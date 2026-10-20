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
// Two rows are not here. "Allocate until OOM-killed" belongs to the OOM
// classification, which reads the kernel's own record and lands separately;
// the fake has the 'allocate' misbehaviour ready for it. "Die holding a
// process-shared lock" has nothing to hold: the design has no process-shared
// lock (docs/PLAN.md §4.6), and tools/check-test-obligations.py makes that a
// checked fact rather than a remembered one.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include "platform/syscalls.hpp"

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
};

bool has_record(const WorkerReport& worker, std::string_view prefix) {
  for (const std::string& record : worker.records) {
    if (record.substr(0, prefix.size()) == prefix) {
      return true;
    }
  }
  return false;
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
