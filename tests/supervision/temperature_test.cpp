// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7, the temperature ceiling (limits.max_temperature_c) with real workers: a
// fake thermal source -- the real thermal source, reading a directory this test
// built through the roots seam in place of /sys -- crosses the ceiling in the
// middle of a run, and the real worker processes are stopped, reaped, and
// reported. The sensor's file is changed from a thread (controller level) or
// from a helper process (command level) started once every worker exists, so
// nothing is forked from a process with a second thread (docs/PLAN.md section
// 4.6).

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "controller/journal/file_ops.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "core/duration.hpp"
#include "main/cli.hpp"
#include "main/journal_rig.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::controller {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kMs = 1'000'000;

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A machine whose only sensor is a coretemp package, in a directory of the test's own.
class FakeMachine {
 public:
  FakeMachine()
      : root_(fs::temp_directory_path() / ("loadforge-ceiling-" + std::to_string(::getpid()) + "-" +
                                           std::to_string(counter_++))) {
    fs::create_directories(chip());
    std::ofstream(chip() / "name") << "coretemp\n";
    std::ofstream(chip() / "temp1_label") << "Package id 0\n";
    set_celsius(40.0);
  }
  ~FakeMachine() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  FakeMachine(const FakeMachine&) = delete;
  FakeMachine(FakeMachine&&) = delete;
  FakeMachine& operator=(const FakeMachine&) = delete;
  FakeMachine& operator=(FakeMachine&&) = delete;

  [[nodiscard]] const fs::path& root() const { return root_; }
  [[nodiscard]] fs::path chip() const { return root_ / "sys" / "class" / "hwmon" / "hwmon0"; }

  /// The sensor now reads this, replaced whole so no reader sees half a number.
  void set_celsius(double celsius) const {
    const fs::path next = chip() / "temp1_input.next";
    std::ofstream(next) << static_cast<long>(celsius * 1000.0) << "\n";
    fs::rename(next, chip() / "temp1_input");
  }
  /// The sensor is gone, as after a driver is unloaded.
  void unplug() const { fs::remove(chip() / "temp1_input"); }

 private:
  static inline int counter_ = 0;
  fs::path root_;
};

/// Changes the machine `after` the workers exist, from a thread that is started
/// once the controller has forked them all (the "spawned" event comes after).
class Heating final : public Reporter {
 public:
  Heating(const FakeMachine& machine, std::chrono::milliseconds after, bool unplug,
          double celsius = 95.0)
      : machine_(&machine), after_(after), unplug_(unplug), celsius_(celsius) {}
  ~Heating() override {
    if (changer_.joinable()) {
      changer_.join();
    }
  }
  Heating(const Heating&) = delete;
  Heating(Heating&&) = delete;
  Heating& operator=(const Heating&) = delete;
  Heating& operator=(Heating&&) = delete;

  void run_starting(const RunPlan& plan) override { limits = plan.limits; }
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    events.emplace_back(line);
    if (line.substr(0, 7) == "spawned") {
      changer_ = std::thread([this] {
        std::this_thread::sleep_for(after_);
        if (unplug_) {
          machine_->unplug();
        } else {
          machine_->set_celsius(celsius_);
        }
      });
    }
  }
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}

  std::vector<LimitStatus> limits;
  std::vector<std::string> events;

 private:
  const FakeMachine* machine_;
  std::chrono::milliseconds after_;
  bool unplug_;
  double celsius_;
  std::thread changer_;
};

struct Ceiling {
  platform::RealSyscalls syscalls;
  platform::FileSystem files{syscalls};
  std::unique_ptr<cli::Rig> rig;
  config::Config config;
  RunOptions options;

  explicit Ceiling(const FakeMachine& machine) {
    auto made = cli::make_rig(syscalls, files, cli::roots_under(machine.root().string()), 100'000);
    EXPECT_TRUE(made.has_value());
    rig = std::move(made).value();
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{60'000};  // Far longer than the test lets it run.
    config.limits.max_temperature_c = 85;
    config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
    config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
    options.executable = LOADFORGE_BINARY;
    options.interval_ns = 20 * kMs;
    options.stale_after_ns = 2000 * kMs;
    options.grace = Grace{2000 * kMs, 2000 * kMs};
    options.sampler = &rig->sampler();
    options.timeline = &rig->timeline();
    const telemetry::TimelineView& readings = rig->timeline();
    options.view = &readings;
    options.sample_interval_ns = 50 * kMs;
  }
};

bool gone(pid_t pid) { return ::kill(pid, 0) == -1 && errno == ESRCH; }

TEST(TemperatureSupervision, ASensorThatCrossesTheCeilingMidRunStopsTheRealWorkers) {
  const FakeMachine machine;
  Ceiling run(machine);
  Heating reporter(machine, std::chrono::milliseconds{700}, false);
  Controller controller(run.syscalls, reporter);
  const auto started = std::chrono::steady_clock::now();
  auto report = controller.run(run.config, run.options);
  const auto took = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(report.has_value()) << describe(report.error());

  ASSERT_TRUE(report.value().temperature_stop.has_value());
  const TemperatureStop& stop = *report.value().temperature_stop;
  EXPECT_EQ(stop.cause, StopCause::kReached);
  EXPECT_EQ(stop.sensor, "thermal.package 0 (hwmon:coretemp)");
  EXPECT_EQ(stop.celsius, 95.0);
  EXPECT_EQ(stop.ceiling_c, 85);
  EXPECT_LT(took, std::chrono::seconds{50}) << "ended by the ceiling, not by the 60 s duration";

  // Every worker was asked to stop, and none is left running.
  ASSERT_EQ(report.value().workers.size(), 2U);
  for (const WorkerReport& worker : report.value().workers) {
    EXPECT_EQ(worker.stop.substr(0, 4), "stop") << "asked first, as at the deadline";
    EXPECT_TRUE(gone(worker.pid)) << "worker " << worker.slot << " pid " << worker.pid;
    EXPECT_GE(worker.commands.sent, 1U);
  }
  bool told = false;
  for (const std::string& line : reporter.events) {
    told = told || line.find("temperature ceiling: thermal.package 0 (hwmon:coretemp) read 95.0 C") ==
                       0;
  }
  EXPECT_TRUE(told);
  ASSERT_EQ(reporter.limits.size(), 2U);
  EXPECT_TRUE(reporter.limits[1].enforced);
}

TEST(TemperatureSupervision, ARunBelowTheCeilingRunsToItsDeadlineWithTheCeilingWatching) {
  const FakeMachine machine;
  Ceiling run(machine);
  run.config.run.duration = core::Duration{1200};
  Heating reporter(machine, std::chrono::milliseconds{200}, false, 84.9);
  Controller controller(run.syscalls, reporter);
  auto report = controller.run(run.config, run.options);
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  EXPECT_FALSE(report.value().temperature_stop.has_value()) << "84.9 is below 85";
  EXPECT_TRUE(all_clean(report.value()));
}

TEST(TemperatureSupervision, ASensorThatIsUnpluggedForGoodEndsTheRunBecauseNothingHoldsTheCeiling) {
  const FakeMachine machine;
  Ceiling run(machine);
  run.options.temperature_blind_after_ns = 400 * kMs;
  Heating reporter(machine, std::chrono::milliseconds{500}, true);
  Controller controller(run.syscalls, reporter);
  auto report = controller.run(run.config, run.options);
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  ASSERT_TRUE(report.value().temperature_stop.has_value());
  EXPECT_EQ(report.value().temperature_stop->cause, StopCause::kUnwatched);
  EXPECT_GT(report.value().temperature_stop->blind_ns, 400 * kMs);
  for (const WorkerReport& worker : report.value().workers) {
    EXPECT_TRUE(gone(worker.pid));
  }
}

TEST(TemperatureSupervision, ACeilingWithNoSensorOnTheMachineStartsNothing) {
  const FakeMachine machine;
  machine.unplug();
  Ceiling run(machine);
  Heating reporter(machine, std::chrono::milliseconds{0}, false);
  Controller controller(run.syscalls, reporter);
  auto report = controller.run(run.config, run.options);
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().stage, "temperature");
  EXPECT_TRUE(reporter.events.empty()) << "no worker was spawned to be told";
}

// --- the command, with verified workers, a result and a journal ----------------------------

/// A link to the shipped binary, which is what the workers of a command must be.
class LinkToTheBinary {
 public:
  LinkToTheBinary()
      : path_(fs::temp_directory_path() / ("loadforge-ceiling-link-" + std::to_string(::getpid()))) {
    fs::remove(path_);
    fs::create_symlink(LOADFORGE_BINARY, path_);
  }
  ~LinkToTheBinary() {
    std::error_code ignored;
    fs::remove(path_, ignored);
  }
  LinkToTheBinary(const LinkToTheBinary&) = delete;
  LinkToTheBinary(LinkToTheBinary&&) = delete;
  LinkToTheBinary& operator=(const LinkToTheBinary&) = delete;
  LinkToTheBinary& operator=(LinkToTheBinary&&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  fs::path path_;
};

/// A process of its own that heats the machine after a while: forked before the
/// command starts any thread, and reaped after.
class Helper {
 public:
  Helper(const FakeMachine& machine, unsigned seconds) : pid_(::fork()) {
    if (pid_ == 0) {
      ::sleep(seconds);
      machine.set_celsius(95.0);
      ::_exit(0);
    }
  }
  ~Helper() {
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  Helper(const Helper&) = delete;
  Helper(Helper&&) = delete;
  Helper& operator=(const Helper&) = delete;
  Helper& operator=(Helper&&) = delete;

 private:
  pid_t pid_;
};

TEST(TemperatureSupervision, TheCommandEndsWithItsOwnExitCodeAndSaysWhereItWroteWhyAndWhen) {
  const FakeMachine machine;
  const LinkToTheBinary link;
  const fs::path out_dir = machine.root() / "out";
  fs::create_directories(out_dir);
  const std::string journal_path = (machine.root() / "run.journal").string();
  const std::string where = out_dir.string();
  const std::vector<std::string_view> args{
      "sort",     "--duration", "120s",     "--workers", "2",    "--footprint",      "6MiB",
      "--seed",   "3",          "--max-temperature", "90", "--sample-interval", "100ms",
      "--output", "json,csv",   "--output-dir", where,   "--journal", journal_path};
  cli::StressOptions base;
  base.roots = cli::roots_under(machine.root().string());
  std::ostringstream out;
  std::ostringstream err;
  int code = -1;
  const auto started = std::chrono::steady_clock::now();
  {
    const Helper heater(machine, 2);
    code = cli::stress_test(args, link.path(), "/sys", base, out, err);
  }
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(code, 7) << out.str() << err.str();
  EXPECT_LT(took, std::chrono::seconds{100}) << "stopped by the ceiling, not by the 120 s duration";
  const std::string text = out.str();
  EXPECT_NE(text.find("  limit temperature: enforced (stops the run at 90 C; watching "
                      "thermal.package 0)\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("temperature ceiling: thermal.package 0 (hwmon:coretemp) read 95.0 C, at or "
                      "above the ceiling of 90 C, "),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("  ended by the temperature ceiling: thermal.package 0 (hwmon:coretemp) read "
                      "95.0 C"),
            std::string::npos)
      << "the run's own summary";
  EXPECT_NE(text.find("\nended by the temperature ceiling: thermal.package 0 (hwmon:coretemp) read "
                      "95.0 C"),
            std::string::npos)
      << "and the result's";

  // Every worker was a child of this process: none is left, and no wait finds one.
  errno = 0;
  EXPECT_EQ(::waitpid(-1, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);

  // The result files say it too.
  const std::string json = slurp(out_dir / "run.json");
  EXPECT_NE(json.find("the run was ended by the temperature ceiling: thermal.package 0 "
                      "(hwmon:coretemp) read 95.0 C, at or above the ceiling of 90 C"),
            std::string::npos)
      << json;
  EXPECT_NE(slurp(out_dir / "run.csv").find("temperature ceiling"), std::string::npos);

  // And the journal, replayed as the next start would: which sensor, what it read,
  // the ceiling, and when.
  journal::RealFileOps ops;
  cli::RealJournalFiles files;
  std::ostringstream shown;
  std::ostringstream shown_err;
  ASSERT_EQ(cli::show_journal(journal_path, ops, files, shown, shown_err), 0) << shown_err.str();
  EXPECT_NE(shown.str().find("ended cleanly"), std::string::npos) << shown.str();
  EXPECT_NE(shown.str().find("phase \"ceiling 90 C reached: thermal.package 0 (hwmon:coretemp)\""),
            std::string::npos)
      << shown.str();
  EXPECT_NE(shown.str().find(" 95.0C "), std::string::npos) << shown.str();
}

TEST(TemperatureSupervision, TheCommandBelowItsCeilingIsCleanAndNamesTheSensorsItWatched) {
  const FakeMachine machine;
  const LinkToTheBinary link;
  const std::vector<std::string_view> args{"sort", "--duration", "2s", "--workers", "2",
                                           "--footprint", "6MiB", "--max-temperature", "90",
                                           "--sample-interval", "100ms"};
  cli::StressOptions base;
  base.roots = cli::roots_under(machine.root().string());
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cli::stress_test(args, link.path(), "/sys", base, out, err), 0)
      << out.str() << err.str();
  EXPECT_NE(out.str().find("  limit temperature: enforced (stops the run at 90 C; watching "
                           "thermal.package 0)\n"),
            std::string::npos)
      << out.str();
  EXPECT_EQ(out.str().find("ended by the temperature ceiling"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::controller
