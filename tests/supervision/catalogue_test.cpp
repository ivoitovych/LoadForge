// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7: the misbehaviour catalogue (docs/IMPLEMENTATION.md §4), one test per
// row this build can reach, each against the fake worker
// (tests/support/fake_worker) run by the real controller as its worker
// executable. Every test runs N >= 2 workers, and the grandchild case N = 3:
// one worker misbehaves and the others must be reported for what they did, not
// for what their neighbour did.
//
// No outcome here depends on which of two racing events lands first
// (docs/JOURNAL.md §4.11). Three devices keep it so:
//
//   * The companion workers 'outlast' the misbehaving one: they finish once
//     the controller has reaped every other worker. The run's deadline is
//     thirty seconds away and never comes into it, so a hang is a timeout and
//     never a stop at the deadline that happened to come first.
//   * A misbehaving worker begins with a prelude that waits for two of the
//     controller's beats after its own, so the controller has read its
//     progress before it faults.
//   * Where the controller must not act before the worker is ready (a SIGTERM
//     that must find the signal already ignored; a death that must find the
//     death signal armed), the test waits for the kernel to say so through
//     /proc before it lets the run go on.
//
// "Allocate until OOM-killed" needs a memory limit the controller's OOM
// record reads, which the test makes: a memory cgroup of its own, with this
// process moved into it so the workers are born there and the record finds
// it. Where the machine will not let it (cgroup v2 without delegation and not
// root, among others) that test skips and quotes the kernel's refusal; the
// companion test, a SIGKILL from outside judged against the same kind of
// record, runs wherever a count can be read at all.
//
// One row is not here. "Die holding a process-shared lock" has nothing to
// hold: the design has no process-shared lock (docs/PLAN.md §4.6), and
// tools/check-test-obligations.py makes that a checked fact rather than a
// remembered one.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "controller/supervision.hpp"
#include "core/duration.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "resources/cgroup.hpp"
#include "resources/cgroup_oom_record.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;
/// The deadline nothing here waits for: the safety net, not the mechanism.
constexpr std::int64_t kNeverMs = 30'000;
/// How long a test waits for something the kernel does at once.
constexpr auto kBound = std::chrono::seconds(10);

config::Config workloads(std::size_t count, std::int64_t duration_ms) {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{duration_ms};
  for (std::size_t index = 0; index < count; ++index) {
    config.workloads.push_back(
        config::Workload{"w" + std::to_string(index), config::Threads::all(), true, {}});
  }
  return config;
}

RunOptions fake_options(std::int64_t stale_after_ms) {
  RunOptions options;
  options.executable = LOADFORGE_FAKE_WORKER;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = stale_after_ms * kMs;
  options.grace = Grace{500 * kMs, 500 * kMs};
  return options;
}

/// Selects the fake worker's misbehaviour for the scope of one test. The
/// environment passes through the controller's fork and the worker's exec;
/// the argv, which the controller builds, could not carry it.
class Selector {
 public:
  explicit Selector(const char* value) { ::setenv("LOADFORGE_FAKE_WORKER", value, 1); }
  ~Selector() { ::unsetenv("LOADFORGE_FAKE_WORKER"); }
  Selector(const Selector&) = delete;
  Selector& operator=(const Selector&) = delete;
  Selector(Selector&&) = delete;
  Selector& operator=(Selector&&) = delete;
};

std::vector<pid_t> pids_in(std::string_view line) {
  std::vector<pid_t> pids;
  const std::size_t at = line.find("pids");
  if (at == std::string_view::npos) {
    return pids;
  }
  std::istringstream rest{std::string{line.substr(at + 4)}};
  pid_t pid = 0;
  while (rest >> pid) {
    pids.push_back(pid);
  }
  return pids;
}

/// Keeps every event with its time, and lets a test act at the one moment it
/// can reach into a run: the "spawned" event, before the first tick.
class Recorder final : public Reporter {
 public:
  void run_starting(const RunPlan& /*plan*/) override {}
  void event(std::int64_t elapsed_ns, std::string_view line) override {
    events.emplace_back(elapsed_ns, std::string{line});
    if (line.substr(0, 7) == "spawned" && on_spawned) {
      on_spawned(pids_in(line));
    }
    if (on_line) {
      on_line(line);
    }
  }
  void run_finished(const RunReport& /*report*/) override {}

  /// When `line` was reported, if it was.
  [[nodiscard]] std::optional<std::int64_t> when(std::string_view line) const {
    for (const auto& [elapsed_ns, text] : events) {
      if (text == line) {
        return elapsed_ns;
      }
    }
    return std::nullopt;
  }

  std::vector<std::pair<std::int64_t, std::string>> events;
  std::function<void(const std::vector<pid_t>&)> on_spawned;
  /// Acts on any event, inside the tick that told it: the only way a test
  /// can come between two workers' escalations on the same tick.
  std::function<void(std::string_view)> on_line;
};

bool has_record(const WorkerReport& worker, std::string_view prefix) {
  return std::ranges::any_of(worker.records, [&](const std::string& record) {
    return record.substr(0, prefix.size()) == prefix;
  });
}

std::string records_of(const WorkerReport& worker) {
  std::string text;
  for (const std::string& record : worker.records) {
    text += "\n  " + record;
  }
  return text;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

/// The controller reaped it: no zombie of ours carries the pid any more.
bool reaped(pid_t pid) {
  errno = 0;
  return ::waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD;
}

/// What the kernel reports for `pid` on the `field:` line of its status file.
std::string proc_status(pid_t pid, std::string_view field) {
  std::ifstream status{"/proc/" + std::to_string(pid) + "/status"};
  std::string line;
  while (std::getline(status, line)) {
    if (starts_with(line, field)) {
      return line.substr(field.size());
    }
  }
  return {};
}

/// Waits until `ready` holds for `pid` or the bound passes.
bool wait_until(const std::function<bool()>& ready) {
  const auto deadline = std::chrono::steady_clock::now() + kBound;
  while (!ready()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

/// The worker has exec'd: its argv is the worker's, so the launcher armed the
/// death signal before it (WorkerLauncher::become_worker arms, then execs).
bool has_execd(pid_t pid) {
  std::ifstream file{"/proc/" + std::to_string(pid) + "/cmdline"};
  const std::string cmdline{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  return cmdline.find(std::string{"--worker\0", 9}) != std::string::npos;
}

/// The worker ignores `signal`: the kernel's SigIgn mask has its bit.
bool ignores(pid_t pid, int signal) {
  const std::string mask = proc_status(pid, "SigIgn:");
  if (mask.empty()) {
    return false;
  }
  const std::uint64_t bits = std::stoull(mask, nullptr, 16);
  return ((bits >> static_cast<unsigned>(signal - 1)) & 1U) != 0;
}

/// How a child of this process ended, as far as this process can tell.
struct Ending {
  bool ended = false;         ///< Reaped here, or already reaped by someone else.
  std::optional<int> status;  ///< The status word, when it was reaped here.
};

/// Reaps `pid` as this process's child, within the bound. ECHILD is an ending
/// too: the controller's own reaping loop met it first, as a stranger.
Ending reap_within_bound(pid_t pid) {
  Ending ending;
  (void)wait_until([&ending, pid] {
    int word = 0;
    const pid_t answer = ::waitpid(pid, &word, WNOHANG);
    if (answer == pid) {
      ending = Ending{true, word};
    } else if (answer < 0 && errno == ECHILD) {
      ending = Ending{true, std::nullopt};
    }
    return ending.ended;
  });
  return ending;
}

RunReport run_fake(const config::Config& config, const RunOptions& options, Recorder& recorder) {
  platform::RealSyscalls syscalls;
  Controller controller(syscalls, recorder);
  auto report = controller.run(config, options);
  EXPECT_TRUE(report.has_value()) << describe(report.error());
  if (!report.has_value()) {
    return {};
  }
  EXPECT_FALSE(report.value().worker_exit_code.has_value());
  return report.value();
}

void expect_completed(const WorkerReport& worker) {
  EXPECT_EQ(worker.fate, WorkerFate::kCompleted)
      << "worker " << worker.slot << ": " << describe(worker.fate) << ", " << worker.termination
      << records_of(worker);
  EXPECT_TRUE(has_record(worker, "finished: ")) << records_of(worker);
}

// --- Exit 0: record completion, reap without a zombie -----------------------

TEST(MisbehaviourCatalogue, ExitZeroWithItsRecordIsCompletionAndLeavesNoZombie) {
  const Selector selector{"*=exit0"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  for (const WorkerReport& worker : report.workers) {
    expect_completed(worker);
    EXPECT_EQ(worker.termination, "exited with code 0");
    EXPECT_TRUE(has_record(worker, "started: slot " + std::to_string(worker.slot)));
    EXPECT_TRUE(reaped(worker.pid)) << "worker " << worker.pid << " left a zombie";
  }
  EXPECT_TRUE(all_clean(report));
}

// --- Exit non-zero: a worker error, not a hardware fault --------------------

TEST(MisbehaviourCatalogue, ExitNonZeroIsAWorkerErrorAndNotAFault) {
  const Selector selector{"*=outlast,1=exit-error"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& failed = report.workers[1];
  EXPECT_EQ(failed.fate, WorkerFate::kWorkerError) << describe(failed.fate);
  EXPECT_EQ(failed.termination, "exited with code 1");
  EXPECT_TRUE(has_record(failed, "error: fake worker: a deliberate failure")) << records_of(failed);
  EXPECT_EQ(clean_count(report), 1U);
}

// --- SIGSEGV, SIGBUS, SIGABRT: a fault, the signal named, the timeline kept --

class FaultSignal : public testing::TestWithParam<std::pair<std::string, int>> {};

TEST_P(FaultSignal, IsAFaultNamingTheSignalWithTheTimelineUpToIt) {
  const auto [name, signal] = GetParam();
  const std::string selection = "*=outlast,1=" + name;
  const Selector selector{selection.c_str()};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& faulted = report.workers[1];
  EXPECT_EQ(faulted.fate, WorkerFate::kFault) << describe(faulted.fate);
  // A prefix, because whether a core was written is the machine's business
  // and not the classification's; the fake asks for none.
  EXPECT_TRUE(starts_with(faulted.termination, "killed by signal " + std::to_string(signal)))
      << faulted.termination;
  // The timeline up to the fault: what it said, and the progress the
  // controller read from its beat before it died.
  EXPECT_TRUE(has_record(faulted, "started: slot 1")) << records_of(faulted);
  EXPECT_GE(faulted.progress, 1U);
  EXPECT_FALSE(has_record(faulted, "finished: "));
}

INSTANTIATE_TEST_SUITE_P(MisbehaviourCatalogue, FaultSignal,
                         testing::Values(std::pair<std::string, int>{"segv", SIGSEGV},
                                         std::pair<std::string, int>{"bus", SIGBUS},
                                         std::pair<std::string, int>{"abrt", SIGABRT}),
                         [](const testing::TestParamInfo<std::pair<std::string, int>>& info) {
                           return info.param.first;
                         });

// --- Silent hang: detected by heartbeat staleness, not by end of file -------

TEST(MisbehaviourCatalogue, ASilentHangHoldingItsDescriptorsIsTimedOutByItsHeartbeat) {
  const Selector selector{"*=outlast,1=hang"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& hung = report.workers[1];
  EXPECT_EQ(hung.fate, WorkerFate::kTimedOut) << describe(hung.fate) << records_of(hung);
  EXPECT_EQ(hung.termination, "killed by signal 15");
  // The hung worker held its record pipe open the whole time, so no end of
  // file could have told the controller anything: the stale heartbeat did.
  EXPECT_TRUE(recorder.when("worker 1: sent signal 15 (heartbeat stale)").has_value());
  EXPECT_FALSE(recorder.when("worker 0: sent signal 15 (heartbeat stale)").has_value());
}

// --- Spin without progress: told apart from working slowly ------------------

TEST(MisbehaviourCatalogue, ASpinWithoutProgressIsTimedOutWhileASlowWorkerCompletes) {
  // Worker 0 advances its progress once per quarter of the allowance: slow,
  // and never stuck. Worker 1 beats every interval with a fresh timestamp and
  // the same progress: alive, busy, and going nowhere.
  const Selector selector{"0=slow,1=spin"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  EXPECT_GE(report.workers[0].progress, 2U);
  const WorkerReport& spinning = report.workers[1];
  EXPECT_EQ(spinning.fate, WorkerFate::kTimedOut) << describe(spinning.fate);
  EXPECT_EQ(spinning.termination, "killed by signal 15");
  EXPECT_EQ(spinning.progress, 1U);
  EXPECT_TRUE(recorder.when("worker 1: sent signal 15 (heartbeat stale)").has_value());
  EXPECT_FALSE(recorder.when("worker 0: sent signal 15 (heartbeat stale)").has_value());
}

// --- Close the record pipe early: truncated, never clean --------------------

TEST(MisbehaviourCatalogue, ARecordPipeClosedEarlyIsTruncatedEvidenceDespiteExitZero) {
  const Selector selector{"*=outlast,1=close-records"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& closed = report.workers[1];
  EXPECT_EQ(closed.fate, WorkerFate::kTruncatedEvidence) << describe(closed.fate);
  EXPECT_EQ(closed.termination, "exited with code 0");
  EXPECT_TRUE(has_record(closed, "started: slot 1")) << records_of(closed);
  EXPECT_FALSE(has_record(closed, "finished: "));
  EXPECT_FALSE(all_clean(report));
}

// --- The stop byte to a pipe nobody reads: EPIPE, never death by SIGPIPE -----

TEST(MisbehaviourCatalogue, AStopByteToAPipeNobodyReadsIsEpipeAndNotDeathBySigpipe) {
  // The row "exit just before the deadline stop", made deterministic: worker 1
  // closes its end of the control pipe at once, so when the deadline comes the
  // controller writes its stop byte to a pipe with no reader at all. With
  // SIGPIPE at its default that write kills the controller -- this test
  // process -- by signal 13 (review R-02); ignored, it is EPIPE, which the
  // stop request tolerates. Worker 1 finishes on its own three controller
  // ticks after its duration, well inside a stop grace long enough that no
  // escalation comes into it; worker 0 completes as the protocol says.
  const Selector selector{"0=complete,1=close-control"};
  Recorder recorder;
  RunOptions options = fake_options(2000);
  options.grace = Grace{5000 * kMs, 5000 * kMs};
  const RunReport report = run_fake(workloads(2, 300), options, recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  EXPECT_TRUE(recorder.when("deadline reached: stop requested").has_value());
  for (const WorkerReport& worker : report.workers) {
    expect_completed(worker);
    EXPECT_EQ(worker.termination, "exited with code 0");
  }
  // Worker 1 was alive when the deadline came, so the byte was written to
  // its pipe. Worker 0 may have finished its own duration and been reaped
  // first, which is why only worker 1's step is asserted.
  EXPECT_EQ(report.workers[1].stop, "stop requested");
  EXPECT_FALSE(recorder.when("worker 1: sent signal 15").has_value());
  EXPECT_TRUE(all_clean(report));
}

// --- A truncated record: rejected, because a partial record is not one ------

TEST(MisbehaviourCatalogue, ATruncatedRecordIsRejectedAndTheEvidenceIsTruncated) {
  const Selector selector{"*=outlast,1=truncated-record"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& cut = report.workers[1];
  EXPECT_EQ(cut.fate, WorkerFate::kTruncatedEvidence) << describe(cut.fate);
  EXPECT_EQ(cut.termination, "exited with code 0");
  EXPECT_TRUE(has_record(cut, "record channel: the stream ended inside a frame"))
      << records_of(cut);
  // The frame was a finished record short by one byte; none of it counts.
  EXPECT_FALSE(has_record(cut, "finished: "));
}

// --- A bad checksum: rejected and reported, over a clean exit ---------------

TEST(MisbehaviourCatalogue, ABadChecksumIsAChannelCorruptVerdictOverAnExitZero) {
  const Selector selector{"*=outlast,1=bad-checksum"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(2, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& corrupt = report.workers[1];
  EXPECT_EQ(corrupt.fate, WorkerFate::kChannelCorrupt) << describe(corrupt.fate);
  EXPECT_EQ(corrupt.termination, "exited with code 0");
  EXPECT_TRUE(has_record(corrupt, "record channel: the frame's checksum does not match"))
      << records_of(corrupt);
  EXPECT_FALSE(has_record(corrupt, "finished: "));
}

// --- Ignore SIGTERM: escalation to SIGKILL within the bound -----------------

TEST(MisbehaviourCatalogue, AWorkerIgnoringSigtermIsKilledOneGraceAfterIt) {
  // Worker 1 ignores its control pipe and SIGTERM and beats on: the deadline's
  // stop request goes unanswered, SIGTERM follows a grace later, and SIGKILL a
  // grace after that. Worker 0 answers the request and completes.
  const Selector selector{"0=complete,1=ignore-term"};
  Recorder recorder;
  // The ladder must find SIGTERM already ignored, or the test would be of a
  // worker that died of SIGTERM. The run waits, at the one moment a test can
  // hold it, until the kernel shows the disposition.
  recorder.on_spawned = [](const std::vector<pid_t>& pids) {
    ASSERT_EQ(pids.size(), 2U);
    ASSERT_TRUE(wait_until([pid = pids[1]] { return ignores(pid, SIGTERM); }))
        << "worker " << pids[1] << " never ignored SIGTERM";
  };
  RunOptions options = fake_options(5000);
  const std::int64_t grace_ns = 300 * kMs;
  options.grace = Grace{grace_ns, grace_ns};
  const RunReport report = run_fake(workloads(2, 300), options, recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& stubborn = report.workers[1];
  EXPECT_EQ(stubborn.fate, WorkerFate::kStoppedByController) << describe(stubborn.fate);
  EXPECT_EQ(stubborn.termination, "killed by signal 9");
  EXPECT_EQ(stubborn.stop, "SIGKILL sent");
  const auto term = recorder.when("worker 1: sent signal 15");
  const auto kill = recorder.when("worker 1: sent signal 9");
  ASSERT_TRUE(term.has_value() && kill.has_value());
  // Strictly after the grace (escalation_due is strict), and well within a
  // bound: a ladder that waited for nothing or forever would show here.
  EXPECT_GT(*kill - *term, grace_ns);
  EXPECT_LT(*kill - *term, grace_ns + 5000 * kMs);
}

// --- EXT-01: a SIGKILL the controller attempts is not a SIGKILL it caused ---

/// Waits until `pid` has terminated WITHOUT reaping it: waitid with WNOWAIT is
/// the barrier that leaves a zombie for the controller to find.
bool wait_until_zombie(pid_t pid) {
  return wait_until([pid] {
    siginfo_t info{};
    return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
           info.si_pid == pid;
  });
}

TEST(MisbehaviourCatalogue, AWorkerKilledFromOutsideBeforeTheControllersSigkillIsNotBlamedOnIt) {
  // Both workers ignore SIGTERM, so both are due SIGKILL on the same tick,
  // worker 0 first. In the event that worker 0's SIGKILL tells, this test
  // kills worker 1 from outside and holds it unreaped (WNOWAIT): it is a
  // zombie when the controller's turn for worker 1 comes, and kill(2) on a
  // zombie succeeds. Before EXT-01 was fixed the controller recorded that
  // success and reported the outside kill as its own stop.
  const Selector selector{"*=ignore-term"};
  Recorder recorder;
  std::vector<pid_t> pids;
  recorder.on_spawned = [&pids](const std::vector<pid_t>& spawned) {
    ASSERT_EQ(spawned.size(), 2U);
    pids = spawned;
    for (const pid_t pid : spawned) {
      ASSERT_TRUE(wait_until([pid] { return ignores(pid, SIGTERM); }))
          << "worker " << pid << " never ignored SIGTERM";
    }
  };
  bool killed_from_outside = false;
  recorder.on_line = [&](std::string_view line) {
    if (line == "worker 0: sent signal 9") {
      ASSERT_EQ(pids.size(), 2U);
      ASSERT_EQ(::kill(pids[1], SIGKILL), 0);
      ASSERT_TRUE(wait_until_zombie(pids[1])) << "worker 1 never terminated";
      killed_from_outside = true;
    }
  };
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  resources::CgroupOomKillRecord record(filesystem, resources::kMachineRoot);
  const bool counted = record.read().has_value();
  RunOptions options = fake_options(5000);
  options.grace = Grace{300 * kMs, 300 * kMs};
  if (counted) {
    options.oom_record = &record;
  }
  const RunReport report = run_fake(workloads(2, 300), options, recorder);
  ASSERT_TRUE(killed_from_outside);
  ASSERT_EQ(report.workers.size(), 2U);

  const WorkerReport& outside = report.workers[1];
  EXPECT_EQ(outside.termination, "killed by signal 9");
  EXPECT_NE(outside.fate, WorkerFate::kStoppedByController) << records_of(outside);
  EXPECT_NE(outside.fate, WorkerFate::kTimedOut) << records_of(outside);
  EXPECT_EQ(outside.stop, "SIGTERM sent") << "no SIGKILL was sent to a worker already dead";
  EXPECT_FALSE(recorder.when("worker 1: sent signal 9").has_value());
  EXPECT_TRUE(has_record(outside, "signal 9 not sent: the worker had already exited"))
      << records_of(outside);
  if (counted) {
    // The count was consulted for it, whichever way it moved.
    EXPECT_TRUE(has_record(outside, "not attributed to the OOM killer: ") ||
                has_record(outside, "OOM kill: "))
        << records_of(outside);
  }
  // The companion in the same run: the controller's own kill of a worker that
  // was running is still its stop, when the count is unconsulted or unmoved.
  const WorkerReport& controllers = report.workers[0];
  EXPECT_EQ(controllers.termination, "killed by signal 9");
  EXPECT_EQ(controllers.stop, "SIGKILL sent");
  if (!counted) {
    EXPECT_EQ(controllers.fate, WorkerFate::kStoppedByController) << records_of(controllers);
  } else {
    EXPECT_TRUE(controllers.fate == WorkerFate::kStoppedByController ||
                controllers.fate == WorkerFate::kKilled ||
                controllers.fate == WorkerFate::kOomKilled)
        << "only a count that moved during the run may change it: " << records_of(controllers);
  }
}

TEST(MisbehaviourCatalogue, TheControllersOwnKillOfARunningWorkerIsStillItsStop) {
  // The companion without any outside hand: both workers ignore SIGTERM and
  // are killed by the controller while running. Nothing moves the count, so
  // with a record offered the count is read and the attempt stands.
  const Selector selector{"*=ignore-term"};
  Recorder recorder;
  recorder.on_spawned = [](const std::vector<pid_t>& spawned) {
    ASSERT_EQ(spawned.size(), 2U);
    for (const pid_t pid : spawned) {
      ASSERT_TRUE(wait_until([pid] { return ignores(pid, SIGTERM); }))
          << "worker " << pid << " never ignored SIGTERM";
    }
  };
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  resources::CgroupOomKillRecord record(filesystem, resources::kMachineRoot);
  const bool counted = record.read().has_value();
  RunOptions options = fake_options(5000);
  options.grace = Grace{300 * kMs, 300 * kMs};
  if (counted) {
    options.oom_record = &record;
  }
  const RunReport report = run_fake(workloads(2, 300), options, recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  for (const WorkerReport& worker : report.workers) {
    EXPECT_EQ(worker.termination, "killed by signal 9");
    EXPECT_EQ(worker.stop, "SIGKILL sent");
    EXPECT_EQ(worker.fate, WorkerFate::kStoppedByController)
        << describe(worker.fate) << records_of(worker);
    EXPECT_FALSE(has_record(worker, "signal 9 not sent")) << records_of(worker);
  }
}

// --- Fork a grandchild, then die: no orphan left holding a core -------------

TEST(MisbehaviourCatalogue, GrandchildrenOfDeadWorkersAreKilledWithThem) {
  // Three workers: two fork a grandchild that spins on a core holding every
  // descriptor it inherited, record pipe included, and then exit; the third
  // outlasts them. This process is the subreaper the grandchildren fall to, so
  // it can see each one end.
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
  const Selector selector{"*=outlast,1=orphan,2=orphan"};
  Recorder recorder;
  const RunReport report = run_fake(workloads(3, kNeverMs), fake_options(2000), recorder);
  ASSERT_EQ(report.workers.size(), 3U);
  expect_completed(report.workers[0]);
  std::vector<pid_t> grandchildren;
  for (std::size_t slot = 1; slot < 3; ++slot) {
    const WorkerReport& parent = report.workers[slot];
    EXPECT_EQ(parent.fate, WorkerFate::kWorkerError) << describe(parent.fate);
    for (const std::string& record : parent.records) {
      constexpr std::string_view kPrefix = "error: forked grandchild ";
      if (starts_with(record, kPrefix)) {
        grandchildren.push_back(std::stoi(record.substr(kPrefix.size())));
      }
    }
  }
  ASSERT_EQ(grandchildren.size(), 2U);
  for (const pid_t grandchild : grandchildren) {
    // Either the controller's reaping loop met it first (it is this process's
    // child now, and a stranger to the pool), or it is reaped here; either
    // way it must be dead, and by SIGKILL if it is reaped here.
    const Ending ending = reap_within_bound(grandchild);
    EXPECT_TRUE(ending.ended) << "grandchild " << grandchild << " outlived its worker";
    if (!ending.ended) {
      (void)::kill(grandchild, SIGKILL);
      (void)::waitpid(grandchild, nullptr, 0);
    } else if (ending.status.has_value()) {
      EXPECT_TRUE(WIFSIGNALED(*ending.status) && WTERMSIG(*ending.status) == SIGKILL)
          << "status " << *ending.status;
    } else {
      EXPECT_TRUE(::kill(grandchild, 0) != 0 && errno == ESRCH);
    }
  }
  // And the controller says what it did: each worker's group still had the
  // grandchild in it when the worker was reaped; the companion's was empty.
  for (std::size_t slot = 1; slot < 3; ++slot) {
    EXPECT_TRUE(has_record(report.workers[slot], "process group: killed what the worker left"))
        << records_of(report.workers[slot]);
  }
  EXPECT_FALSE(has_record(report.workers[0], "process group: "));
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 0), 0);
}

// --- Allocate until OOM-killed: told from a fault by the kernel's record ----

/// Writes `value` to a cgroup control file. Nothing when the kernel took it;
/// otherwise the file and what the kernel said, for a skip to quote.
std::optional<std::string> write_control(const std::string& path, std::string_view value) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return path + ": " + std::strerror(errno);
  }
  const ssize_t written = ::write(fd, value.data(), value.size());
  const int error = errno;
  ::close(fd);
  if (written < 0) {
    return path + ": " + std::strerror(error);
  }
  if (written != static_cast<ssize_t>(value.size())) {
    return path + ": a short write";
  }
  return std::nullopt;
}

/// SwapTotal from /proc/meminfo, in kB; nothing when it cannot be read.
std::optional<std::uint64_t> swap_total_kb() {
  std::ifstream meminfo{"/proc/meminfo"};
  std::string line;
  while (std::getline(meminfo, line)) {
    if (starts_with(line, "SwapTotal:")) {
      return std::stoull(line.substr(10));
    }
  }
  return std::nullopt;
}

/// Set where the OOM row must run rather than skip: CI's root step.
constexpr const char* kRequireLimit = "LOADFORGE_REQUIRE_OOM_LIMIT";

/// A memory cgroup made for one test, with this process moved into it, so the
/// workers the controller forks are born there and the controller's OOM-kill
/// record, located from /proc/self/cgroup when it is made, reads its count.
/// Undone on destruction: this process goes home and the cgroup is removed.
///
/// WHERE IT IS MADE
/// ----------------
/// v1 lets a cgroup that holds processes have children, so the new one is a
/// child of this process's own; its limit binds, and its count is its own
/// (docs/JOURNAL.md §1.24). v2 does not ("no internal processes": a cgroup
/// holding processes cannot enable the memory controller for children), so
/// the new one is a SIBLING, under the parent that already enables memory for
/// this process's cgroup. Either needs write access to the hierarchy: root, or
/// a delegated v2 subtree. Without it the kernel refuses, and `unavailable`
/// carries its words for the skip.
class LimitedCgroup {
 public:
  explicit LimitedCgroup(std::uint64_t limit_bytes) {
    platform::RealSyscalls syscalls;
    platform::FileSystem filesystem(syscalls);
    auto located = resources::locate_memory_cgroup(filesystem, resources::kMachineRoot);
    if (!located.has_value()) {
      unavailable_ =
          "this process's memory cgroup cannot be located: " + topology::describe(located.error());
      return;
    }
    if (!located.value().has_value()) {
      unavailable_ = "this process is in no memory cgroup, so nothing can limit a worker";
      return;
    }
    const resources::MemoryCgroup& own = *located.value();
    version_ = own.version;
    home_ = resources::directory_of(own);
    const std::string kind{resources::describe(version_)};
    // Where to try, in order. v1: this process's own cgroup. v2: its parent,
    // which already enables memory for it, and then the hierarchy's root,
    // which the no-internal-processes rule exempts and which root may write
    // where the parent is not delegated.
    std::vector<std::string> parents;
    if (version_ == resources::CgroupVersion::kV1) {
      parents.push_back(home_);
    } else if (!own.relative.empty()) {
      parents.push_back(home_.substr(0, home_.rfind('/')));
      if (parents.back() != own.mount) {
        parents.push_back(own.mount);
      }
    }
    if (parents.empty()) {
      unavailable_ = kind + ": this process is in the root of the hierarchy it can see (" + home_ +
                     "), which holds processes and so cannot give a child the memory "
                     "controller, and has no parent to make a sibling under";
      return;
    }
    std::string refusals;
    for (const std::string& parent : parents) {
      const std::optional<std::string> refused = make_under(parent, limit_bytes);
      if (!refused.has_value()) {
        break;
      }
      refusals += (refusals.empty() ? "" : "; ") + *refused;
    }
    if (!made_) {
      unavailable_ =
          kind + ": " + refusals + " (this needs root, or a delegated cgroup v2 subtree)";
      return;
    }
    if (auto refused = write_control(directory_ + "/cgroup.procs", std::to_string(::getpid()))) {
      unavailable_ = kind + ": cannot move this process into the cgroup: " + *refused;
      return;
    }
    moved_ = true;
  }

  ~LimitedCgroup() {
    if (moved_) {
      if (auto refused = write_control(home_ + "/cgroup.procs", std::to_string(::getpid()))) {
        ADD_FAILURE() << "could not move this process back to " << home_ << ": " << *refused;
      }
    }
    if (made_) {
      // The cgroup empties as its last dead process is released.
      bool removed = false;
      for (int attempt = 0; attempt < 200 && !removed; ++attempt) {
        removed = ::rmdir(directory_.c_str()) == 0;
        if (!removed) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
      EXPECT_TRUE(removed) << "could not remove " << directory_ << ": " << std::strerror(errno);
    }
  }

  LimitedCgroup(const LimitedCgroup&) = delete;
  LimitedCgroup& operator=(const LimitedCgroup&) = delete;
  LimitedCgroup(LimitedCgroup&&) = delete;
  LimitedCgroup& operator=(LimitedCgroup&&) = delete;

  /// Why it could not be made, in the kernel's words; empty when it was.
  [[nodiscard]] const std::string& unavailable() const { return unavailable_; }
  [[nodiscard]] const std::string& directory() const { return directory_; }
  [[nodiscard]] resources::CgroupVersion version() const { return version_; }

 private:
  /// A cgroup under `parent`, limited and kept off swap. Nothing when it was
  /// made (and directory_ names it); otherwise why not, with nothing left.
  std::optional<std::string> make_under(const std::string& parent, std::uint64_t limit_bytes) {
    const bool v1 = version_ == resources::CgroupVersion::kV1;
    const std::string directory = parent + "/loadforge-oom-" + std::to_string(::getpid());
    if (::mkdir(directory.c_str(), 0755) != 0) {
      return "cannot make " + directory + ": " + std::strerror(errno);
    }
    const std::string limit_file = directory + (v1 ? "/memory.limit_in_bytes" : "/memory.max");
    if (auto refused = write_control(limit_file, std::to_string(limit_bytes))) {
      (void)::rmdir(directory.c_str());
      return "cannot set the memory limit: " + *refused;
    }
    // Off swap, or the allocator would be swapped out rather than killed.
    // Where the file cannot be written, a machine with no swap is still fine.
    const std::string swap_file = directory + (v1 ? "/memory.swappiness" : "/memory.swap.max");
    if (auto refused = write_control(swap_file, "0")) {
      const std::optional<std::uint64_t> swap = swap_total_kb();
      if (!swap.has_value() || *swap != 0) {
        (void)::rmdir(directory.c_str());
        return "cannot keep the cgroup off swap on a machine with swap: " + *refused;
      }
    }
    directory_ = directory;
    made_ = true;
    return std::nullopt;
  }

  resources::CgroupVersion version_ = resources::CgroupVersion::kV2;
  std::string home_;
  std::string directory_;
  bool made_ = false;
  bool moved_ = false;
  std::string unavailable_;
};

TEST(MisbehaviourCatalogue, AWorkerOomKilledUnderALimitIsOomKilledAndNotAFaultOrAKill) {
  // M1 exit criteria 2 and 3, end to end. Three fake workers in a cgroup
  // limited to 256 MiB: worker 1 allocates until the kernel's OOM killer ends
  // it, worker 2 dies by SIGSEGV, worker 0 outlasts both. All three deaths
  // are real, the record is the real cgroup's real count, and the controller
  // must tell the two deaths apart: the SIGKILL nobody in the run sent is the
  // OOM killer's, because the count moved by one for it; the SIGSEGV is a
  // fault, however the count moved.
  //
  // Nothing here races: the allocator raises its own oom_score_adj to 1000
  // first, so whichever process charged the page that crossed the limit, the
  // kernel's choice is the allocator; the companion and the controller stay
  // far below it.
  constexpr std::uint64_t kLimitBytes = std::uint64_t{256} << 20U;
  const LimitedCgroup cgroup{kLimitBytes};
  if (!cgroup.unavailable().empty()) {
    // CI runs this test a second time as root, where a skip would be a
    // regression rather than a fact about the machine, and says so.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (std::getenv(kRequireLimit) != nullptr) {
      FAIL() << kRequireLimit << " is set and the workers cannot be put under a memory limit: "
             << cgroup.unavailable();
    }
    GTEST_SKIP() << "cannot put the workers under a memory limit: " << cgroup.unavailable();
  }
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  // Made now, from inside the cgroup, as `loadforge stress` makes it: located
  // from /proc/self/cgroup, so it must be the new cgroup's count.
  resources::CgroupOomKillRecord record(filesystem, resources::kMachineRoot);
  ASSERT_EQ(record.scope(),
            cgroup.directory() + "/" + std::string{resources::oom_count_file(cgroup.version())});
  auto before = record.read();
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (!before.has_value() && before.error().availability == topology::Availability::kAbsent &&
      std::getenv(kRequireLimit) == nullptr) {
    GTEST_SKIP() << "this kernel keeps no OOM-kill count: " << topology::describe(before.error());
  }
  ASSERT_TRUE(before.has_value()) << topology::describe(before.error());
  ASSERT_EQ(before.value(), 0U) << "a cgroup made for this test has killed nothing yet";

  const Selector selector{"*=outlast,1=allocate,2=segv"};
  Recorder recorder;
  RunOptions options = fake_options(2000);
  options.oom_record = &record;
  const RunReport report = run_fake(workloads(3, kNeverMs), options, recorder);
  ASSERT_EQ(report.workers.size(), 3U);
  expect_completed(report.workers[0]);
  EXPECT_FALSE(has_record(report.workers[0], "OOM kill: ")) << records_of(report.workers[0]);

  const WorkerReport& oom = report.workers[1];
  EXPECT_EQ(oom.fate, WorkerFate::kOomKilled) << describe(oom.fate) << records_of(oom);
  EXPECT_EQ(oom.termination, "killed by signal 9");
  EXPECT_EQ(oom.stop, "running") << "the controller asked nothing of it";
  EXPECT_TRUE(has_record(oom, "started: slot 1")) << records_of(oom);
  EXPECT_GE(oom.progress, 1U);
  EXPECT_FALSE(has_record(oom, "finished: "));
  EXPECT_FALSE(has_record(oom, "error: ")) << "it was killed, not refused" << records_of(oom);
  EXPECT_TRUE(has_record(oom, "OOM kill: " + record.scope() +
                                  " counted 1 during the run, one or more for each of the 1 "
                                  "worker(s) killed by a SIGKILL"))
      << records_of(oom);

  const WorkerReport& faulted = report.workers[2];
  EXPECT_EQ(faulted.fate, WorkerFate::kFault) << describe(faulted.fate) << records_of(faulted);
  EXPECT_TRUE(starts_with(faulted.termination, "killed by signal " + std::to_string(SIGSEGV)))
      << faulted.termination;
  EXPECT_FALSE(has_record(faulted, "OOM kill: ")) << records_of(faulted);
  EXPECT_EQ(clean_count(report), 1U);

  // The kernel's own word for it, read again after the run: one kill.
  auto after = record.read();
  ASSERT_TRUE(after.has_value()) << topology::describe(after.error());
  EXPECT_EQ(after.value(), 1U);
}

TEST(MisbehaviourCatalogue, ASigkillFromOutsideIsAKillWhenTheRecordCountedNone) {
  // The other half of telling them apart: the same record consulted, a
  // SIGKILL nobody in the run sent, and no OOM kill counted for it. It must
  // stay a kill, and its report must say the count was looked at. This needs
  // no limit, only a count to read, so it runs wherever the machine keeps one.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  resources::CgroupOomKillRecord record(filesystem, resources::kMachineRoot);
  if (auto count = record.read(); !count.has_value()) {
    GTEST_SKIP() << "no OOM-kill count to consult on this machine: "
                 << topology::describe(count.error());
  }
  const Selector selector{"1=deaf"};
  Recorder recorder;
  recorder.on_spawned = [](const std::vector<pid_t>& pids) {
    ASSERT_EQ(pids.size(), 2U);
    ASSERT_TRUE(wait_until([pid = pids[1]] { return has_execd(pid); }))
        << "worker " << pids[1] << " never exec'd";
    ASSERT_EQ(::kill(pids[1], SIGKILL), 0);
  };
  RunOptions options = fake_options(2000);
  options.oom_record = &record;
  const RunReport report = run_fake(workloads(2, 600), options, recorder);
  ASSERT_EQ(report.workers.size(), 2U);
  expect_completed(report.workers[0]);
  const WorkerReport& killed = report.workers[1];
  EXPECT_EQ(killed.fate, WorkerFate::kKilled) << describe(killed.fate) << records_of(killed);
  EXPECT_EQ(killed.termination, "killed by signal 9");
  EXPECT_TRUE(has_record(killed, "not attributed to the OOM killer: " + record.scope() +
                                     " counted no OOM kill during the run"))
      << records_of(killed);
}

// --- The controller killed: only the parent-death signal is left ------------

TEST(MisbehaviourCatalogue, AWorkerDeafToItsControlPipeIsEndedByTheParentDeathSignal) {
  // The deaf worker never reads its control pipe and never stops on its own,
  // so when the controller is SIGKILLed there is no hang-up for it to notice
  // and nothing else to end it: if it dies, the death signal the launcher
  // armed killed it. This is the test AControllerKilledLeavesNoWorkerBehind
  // cannot be, because a real worker may leave on the hang-up first.
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
  const Selector selector{"*=deaf"};
  int fds[2] = {-1, -1};
  // Close-on-exec, or the workers would hold the write end (docs/JOURNAL.md
  // §1.22) and the end of file read below would never come.
  ASSERT_EQ(::pipe2(fds, O_CLOEXEC), 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::close(fds[0]);
    platform::RealSyscalls syscalls;
    Recorder recorder;
    recorder.on_spawned = [write_fd = fds[1]](const std::vector<pid_t>& pids) {
      std::string line;
      for (const pid_t pid : pids) {
        line += std::to_string(pid) + "\n";
      }
      (void)::write(write_fd, line.data(), line.size());
      ::close(write_fd);
    };
    Controller controller(syscalls, recorder);
    (void)controller.run(workloads(2, 60'000), fake_options(2000));
    ::_exit(0);
  }
  ::close(fds[1]);
  std::string text;
  char buffer[128];
  for (ssize_t count = ::read(fds[0], buffer, sizeof buffer); count > 0;
       count = ::read(fds[0], buffer, sizeof buffer)) {
    text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  std::istringstream lines{text};
  std::vector<pid_t> workers;
  pid_t pid = 0;
  while (lines >> pid) {
    workers.push_back(pid);
  }
  ASSERT_EQ(workers.size(), 2U) << "pids reported: '" << text << "'";
  // Killed before it armed the signal, a worker would exit 121 instead: the
  // launcher's orphan check. Once it has exec'd, the signal is armed.
  for (const pid_t worker : workers) {
    EXPECT_TRUE(wait_until([worker] { return has_execd(worker); }))
        << "worker " << worker << " never exec'd";
  }

  ASSERT_EQ(::kill(child, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  for (const pid_t worker : workers) {
    // The orphans fall to this process, the subreaper, and nobody else reaps
    // here: an ending without a status would be a worker that was never ours.
    const Ending ending = reap_within_bound(worker);
    if (!ending.ended) {
      ADD_FAILURE() << "worker " << worker << " outlived the controller";
      (void)::kill(worker, SIGKILL);
      (void)::waitpid(worker, nullptr, 0);
      continue;
    }
    if (!ending.status.has_value()) {
      ADD_FAILURE() << "worker " << worker << " was not this process's to reap";
      continue;
    }
    // Exactly SIGKILL, the launcher's kParentDeathSignal, and nothing else:
    // this worker had no other way out.
    EXPECT_TRUE(WIFSIGNALED(*ending.status) && WTERMSIG(*ending.status) == SIGKILL)
        << "status " << *ending.status;
  }
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 0), 0);
}

}  // namespace
}  // namespace loadforge::controller
