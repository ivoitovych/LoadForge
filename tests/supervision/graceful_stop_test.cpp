// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7, a signal stops a run gracefully (journal 34b): the shipped binary is sent SIGINT while it
// runs, and a run that reaches no deadline still reports, writes its result and ends its
// journal cleanly, with the shell's exit code for the signal; sent two signals it does the
// same. Beneath it, the controller in this process over really spawned workers that ignore
// SIGTERM and their control pipe: the first signal asks, the second kills them at once, and the
// grace that would have kept them for minutes is never waited out.
//
// Nothing here asserts how long anything takes. The waits are for a thing to have happened
// (the console to say the workers exist, the process to have ended), with a bound that only a
// run that ignored the signal would reach.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "controller/stop_signals.hpp"
#include "core/duration.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::controller {
namespace {

namespace fs = std::filesystem;
using std::chrono::seconds;

constexpr std::int64_t kMs = 1'000'000;
/// A bound only a run that never stopped would pass.
constexpr auto kBound = seconds(120);

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

bool wait_until(const std::function<bool()>& ready, std::chrono::seconds bound = kBound) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (!ready()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

std::string run_and_capture(const std::vector<std::string>& args) {
  std::string command;
  for (const std::string& each : args) {
    command += "'" + each + "' ";
  }
  command += "2>&1";
  std::string out;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return out;
  }
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    out += buffer.data();
  }
  ::pclose(pipe);
  return out;
}

/// A scratch directory of this test's own.
class Scratch {
 public:
  Scratch() {
    std::string pattern = (fs::temp_directory_path() / "loadforge-graceful-XXXXXX").string();
    EXPECT_NE(::mkdtemp(pattern.data()), nullptr);
    path_ = pattern;
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch(Scratch&&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  Scratch& operator=(Scratch&&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

/// The shipped binary, running `stress --test sort` for a minute with a journal and a JSON
/// result, its console in a file. Not waited for: the test sends it its signals.
class RunningBinary {
 public:
  explicit RunningBinary(const Scratch& scratch)
      : console_(scratch.path() / "console.txt"),
        journal_((scratch.path() / "run.lfj").string()),
        results_(scratch.path() / "results") {
    const std::vector<std::string> args = {
        LOADFORGE_BINARY, "stress", "--test",       "sort",           "--duration", "60s",
        "--workers",      "2",      "--footprint",  "8MiB",           "--journal",  journal_,
        "--output",       "json",   "--output-dir", results_.string()};
    fs::create_directories(results_);
    std::vector<char*> argv(args.size() + 1, nullptr);
    std::transform(args.begin(), args.end(), argv.begin(),
                   [](const std::string& each) { return const_cast<char*>(each.c_str()); });
    posix_spawn_file_actions_t actions;
    EXPECT_EQ(::posix_spawn_file_actions_init(&actions), 0);
    EXPECT_EQ(::posix_spawn_file_actions_addopen(&actions, 1, console_.c_str(),
                                                 O_WRONLY | O_CREAT | O_TRUNC, 0600),
              0);
    EXPECT_EQ(::posix_spawn_file_actions_adddup2(&actions, 1, 2), 0);
    // A process group of its own, as a job at a terminal has: Ctrl-C is a SIGINT to the whole
    // group (send_to_group), and the workers, which lead sessions of their own, are not in it.
    posix_spawnattr_t attributes;
    EXPECT_EQ(::posix_spawnattr_init(&attributes), 0);
    EXPECT_EQ(::posix_spawnattr_setpgroup(&attributes, 0), 0);
    EXPECT_EQ(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP), 0);
    EXPECT_EQ(::posix_spawn(&pid_, LOADFORGE_BINARY, &actions, &attributes, argv.data(), environ),
              0);
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
  }
  ~RunningBinary() {
    if (!ended_) {
      ::kill(pid_, SIGKILL);  // Its workers leave with it.
      int ignored = 0;
      ::waitpid(pid_, &ignored, 0);
    }
  }
  RunningBinary(const RunningBinary&) = delete;
  RunningBinary(RunningBinary&&) = delete;
  RunningBinary& operator=(const RunningBinary&) = delete;
  RunningBinary& operator=(RunningBinary&&) = delete;

  /// Whether the console has said the workers exist, which is when the run is under way.
  [[nodiscard]] bool running() const { return contains(slurp(console_), "spawned 2 worker(s)"); }
  /// Whether the process has ended already, before anything was sent to it: then there is
  /// nothing to wait for, and its console says why. The status is kept for wait().
  [[nodiscard]] bool gone() {
    if (!early_.has_value()) {
      int word = 0;
      if (::waitpid(pid_, &word, WNOHANG) == pid_) {
        early_ = word;
        ended_ = true;
      }
    }
    return early_.has_value();
  }
  [[nodiscard]] bool said(std::string_view part) const { return contains(slurp(console_), part); }
  void send(int signal) const { EXPECT_EQ(::kill(pid_, signal), 0); }
  /// What the terminal does with Ctrl-C: every process of the foreground group.
  void send_to_group(int signal) const { EXPECT_EQ(::kill(-pid_, signal), 0); }

  /// The status word, once the process has ended on its own.
  [[nodiscard]] std::optional<int> wait() {
    std::optional<int> status = early_;
    (void)wait_until([&] {
      int word = 0;
      if (::waitpid(pid_, &word, WNOHANG) == pid_) {
        status = word;
      }
      return status.has_value();
    });
    ended_ = status.has_value();
    return status;
  }

  [[nodiscard]] std::string console() const { return slurp(console_); }
  [[nodiscard]] const std::string& journal() const { return journal_; }
  [[nodiscard]] const fs::path& results() const { return results_; }

 private:
  fs::path console_;
  std::string journal_;
  fs::path results_;
  pid_t pid_ = 0;
  bool ended_ = false;
  std::optional<int> early_;
};

/// What every stopped run must show, whichever way it was stopped.
void expect_a_run_that_reported(RunningBinary& run, std::optional<int> status,
                                const std::string& name = "SIGINT", int code = 130) {
  ASSERT_TRUE(status.has_value()) << "the run did not end within the bound: " << run.console();
  ASSERT_TRUE(WIFEXITED(*status)) << "ended by a signal, not by itself: " << run.console();
  EXPECT_EQ(WEXITSTATUS(*status), code) << run.console();
  const std::string console = run.console();
  EXPECT_TRUE(contains(console, name + " received: stop requested")) << console;
  EXPECT_TRUE(contains(console, "run ended after")) << "the console report: " << console;
  EXPECT_TRUE(contains(console, "stopped by " + name)) << console;
  EXPECT_TRUE(contains(console, "its exit code is " + std::to_string(code))) << console;
  EXPECT_TRUE(contains(console, "result written: ")) << console;
  EXPECT_FALSE(contains(console, "ended abnormally")) << console;

  std::vector<fs::path> written;
  for (const auto& entry : fs::directory_iterator(run.results())) {
    written.push_back(entry.path());
  }
  ASSERT_EQ(written.size(), 1U) << "one result file";
  EXPECT_TRUE(contains(slurp(written.front()), "the run was stopped by " + name))
      << "and it says why the run is short";

  const std::string replayed = run_and_capture({LOADFORGE_BINARY, "journal", run.journal()});
  EXPECT_TRUE(contains(replayed, "ended cleanly")) << replayed;
  EXPECT_TRUE(contains(replayed, "phase \"stopped by " + name + "\"")) << replayed;
  EXPECT_TRUE(contains(replayed, "phase \"done\"")) << replayed;
}

TEST(GracefulStop, SigintStopsTheShippedBinaryWhichReportsWritesItsResultAndEndsItsJournal) {
  const Scratch scratch;
  RunningBinary run(scratch);
  ASSERT_TRUE(wait_until([&run] { return run.running() || run.gone(); })) << run.console();
  ASSERT_FALSE(run.gone()) << "it ended before it was signalled: " << run.console();

  run.send(SIGINT);

  expect_a_run_that_reported(run, run.wait());
}

TEST(GracefulStop, ACtrlCToTheWholeGroupReachesTheControllerAndNotTheWorkersWhichLeadSessions) {
  const Scratch scratch;
  RunningBinary run(scratch);
  ASSERT_TRUE(wait_until([&run] { return run.running() || run.gone(); })) << run.console();
  ASSERT_FALSE(run.gone()) << "it ended before it was signalled: " << run.console();

  run.send_to_group(SIGINT);

  // A worker that had been given the terminal's SIGINT would have died of it, and its fate
  // would not be "completed": the report is what shows it did not.
  expect_a_run_that_reported(run, run.wait());
  EXPECT_TRUE(contains(run.console(), "2 of 2 worker(s) completed")) << run.console();
}

TEST(GracefulStop, SigtermStopsItTooAndTheShellsCodeForItIs143) {
  const Scratch scratch;
  RunningBinary run(scratch);
  ASSERT_TRUE(wait_until([&run] { return run.running() || run.gone(); })) << run.console();
  ASSERT_FALSE(run.gone()) << "it ended before it was signalled: " << run.console();

  run.send(SIGTERM);

  expect_a_run_that_reported(run, run.wait(), "SIGTERM", 143);
}

TEST(GracefulStop, TwoSignalsInARowEndItTheSameWayWhetherOrNotTheSecondFoundTheWorkersStillThere) {
  const Scratch scratch;
  RunningBinary run(scratch);
  ASSERT_TRUE(wait_until([&run] { return run.running() || run.gone(); })) << run.console();
  ASSERT_FALSE(run.gone()) << "it ended before it was signalled: " << run.console();

  run.send(SIGINT);
  run.send(SIGTERM);

  // The first signal is the cause; the second may have killed the workers or have come
  // after they had gone. Both are the same run to the shell.
  expect_a_run_that_reported(run, run.wait());
}

// ---- the controller itself, over workers that will not stop ----------------------------------

class Recorder final : public Reporter {
 public:
  void run_starting(const RunPlan& /*plan*/) override {}
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    events.emplace_back(line);
    if (line.substr(0, 7) == "spawned" && on_spawned) {
      on_spawned();
    }
    if (on_line) {
      on_line(line);
    }
  }
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}

  [[nodiscard]] bool told(std::string_view part) const {
    return std::ranges::any_of(
        events, [part](const std::string& event) { return event.find(part) != std::string::npos; });
  }

  std::vector<std::string> events;
  std::function<void()> on_spawned;
  std::function<void(std::string_view)> on_line;
};

class Selector {
 public:
  explicit Selector(const char* value) { ::setenv("LOADFORGE_FAKE_WORKER", value, 1); }
  ~Selector() { ::unsetenv("LOADFORGE_FAKE_WORKER"); }
  Selector(const Selector&) = delete;
  Selector& operator=(const Selector&) = delete;
  Selector(Selector&&) = delete;
  Selector& operator=(Selector&&) = delete;
};

config::Config two_stubborn_workers() {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{600'000};  // Ten minutes: the signal ends it, not this.
  for (std::size_t index = 0; index < 2; ++index) {
    config.workloads.push_back(
        config::Workload{"w" + std::to_string(index), config::Threads::all(), true, {}});
  }
  return config;
}

RunOptions options_with(StopSignals& stop) {
  RunOptions options;
  options.executable = LOADFORGE_FAKE_WORKER;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = 60'000 * kMs;
  // Minutes of grace: a run that waited it out would not end within the bound.
  options.grace = Grace{60'000 * kMs, 60'000 * kMs};
  options.stop_signals = &stop;
  return options;
}

TEST(GracefulStopController, TheFirstSignalAsksAndTheSecondKillsWorkersThatIgnoreEverythingElse) {
  const Selector selector{"*=ignore-term"};
  platform::RealSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  Recorder recorder;
  recorder.on_spawned = [] { ASSERT_EQ(::kill(::getpid(), SIGINT), 0); };
  recorder.on_line = [](std::string_view line) {
    if (line == "SIGINT received: stop requested") {
      ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);  // The user insists.
    }
  };
  Controller controller(syscalls, recorder);

  auto report = controller.run(two_stubborn_workers(), options_with(stop));
  stop.release();

  ASSERT_TRUE(report.has_value()) << describe(report.error());
  EXPECT_EQ(report.value().stop_signal, std::optional<int>{SIGINT});
  EXPECT_TRUE(report.value().stop_forced);
  EXPECT_TRUE(recorder.told("another signal received: killing the workers"));
  EXPECT_FALSE(recorder.told("sent signal 15")) << "no SIGTERM: the grace was not waited for";
  ASSERT_EQ(report.value().workers.size(), 2U);
  for (const WorkerReport& worker : report.value().workers) {
    EXPECT_EQ(worker.termination, "killed by signal 9");
    EXPECT_EQ(worker.fate, WorkerFate::kStoppedByController);
  }
}

TEST(GracefulStopController, ASingleSignalAsksWorkersThatAnswerAndTheyCompleteWithoutBeingKilled) {
  const Selector selector{"*=complete"};
  platform::RealSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  Recorder recorder;
  recorder.on_spawned = [] { ASSERT_EQ(::kill(::getpid(), SIGTERM), 0); };
  Controller controller(syscalls, recorder);

  auto report = controller.run(two_stubborn_workers(), options_with(stop));
  stop.release();

  ASSERT_TRUE(report.has_value()) << describe(report.error());
  EXPECT_EQ(report.value().stop_signal, std::optional<int>{SIGTERM});
  EXPECT_FALSE(report.value().stop_forced);
  EXPECT_TRUE(recorder.told("SIGTERM received: stop requested"));
  EXPECT_TRUE(all_clean(report.value()));
}

}  // namespace
}  // namespace loadforge::controller
