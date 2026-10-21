// SPDX-License-Identifier: GPL-3.0-or-later
//
// The controller's loop over the fake: workers that are pids and pipes the
// test plays from inside the reporter's callbacks, the only place a test can
// act while the loop owns the thread. Every scenario runs two workers.

#include "controller/orchestration.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/safety.hpp"
#include "controller/supervision.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "resources/oom_kill_record.hpp"
#include "support/fake_syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "worker/entry.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::Reaped;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;
constexpr pid_t kFirstPid = 1001;
constexpr pid_t kSecondPid = 1002;
// The fake hands out descriptors in a fixed order: the region, then per worker
// a record pipe and a control pipe.
constexpr int kRegionFd = 100;
constexpr int kRecordRead0 = 200;
constexpr int kRecordWrite0 = 201;
constexpr int kControlRead0 = 202;
constexpr int kControlWrite0 = 203;
constexpr int kRecordRead1 = 204;
constexpr int kRecordWrite1 = 205;
constexpr int kControlRead1 = 206;
constexpr int kControlWrite1 = 207;

/// A reporter whose every callback is a hook the test sets.
class Choreographer final : public Reporter {
 public:
  void run_starting(const RunPlan& plan) override {
    plans.push_back(plan);
    if (on_starting) {
      on_starting();
    }
  }
  void event(std::int64_t elapsed_ns, std::string_view line) override {
    events.emplace_back(line);
    elapsed.push_back(elapsed_ns);
    if (on_event) {
      on_event(std::string{line});
    }
  }
  void run_finished(const RunReport& report) override { reports.push_back(report); }

  std::vector<RunPlan> plans;
  std::vector<std::string> events;
  std::vector<std::int64_t> elapsed;
  std::vector<RunReport> reports;
  std::function<void()> on_starting;
  std::function<void(const std::string&)> on_event;
};

std::string frame_text(ipc::RecordType type, std::string_view payload) {
  std::vector<std::byte> bytes;
  for (const char c : payload) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  auto encoded = ipc::encode(static_cast<std::uint16_t>(type), bytes);
  EXPECT_TRUE(encoded.has_value());
  std::string text;
  for (const std::byte b : encoded.value()) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
}

/// The OOM-kill record as this controller reads its contract: a count, read
/// on demand, that answers from a script and remembers how often it was asked.
/// The controller's own fake (docs/adr/0003); the reader's real one is tested
/// against the kernel in tests/unit/resources.
class ScriptedOomRecord final : public resources::OomKillRecord {
 public:
  [[nodiscard]] std::string scope() const override { return "the scripted count"; }
  core::Result<std::uint64_t, std::string> kills() override {
    ++reads;
    if (readings.empty()) {
      return std::string{"read more often than scripted"};
    }
    auto next = readings.front();
    readings.pop_front();
    return next;
  }

  std::deque<core::Result<std::uint64_t, std::string>> readings;
  int reads = 0;
};

/// The smallest sysfs a topology snapshot accepts, built on disk: one online
/// CPU on its own core in package 0, no cache directories, no NUMA layout.
/// The controller only carries a snapshot into its plan; what discovery reads
/// is the topology module's business and its own tests'.
class OneCpuSysfs {
 public:
  OneCpuSysfs()
      : root_(std::filesystem::temp_directory_path() /
              ("loadforge-controller-sysfs-" + std::to_string(::getpid()))) {
    const std::filesystem::path cpu = root_ / "devices" / "system" / "cpu";
    std::filesystem::create_directories(cpu / "cpu0" / "topology");
    write(cpu / "online", "0");
    write(cpu / "cpu0" / "topology" / "physical_package_id", "0");
    write(cpu / "cpu0" / "topology" / "core_id", "0");
    write(cpu / "cpu0" / "topology" / "thread_siblings_list", "0");
  }
  ~OneCpuSysfs() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  OneCpuSysfs(const OneCpuSysfs&) = delete;
  OneCpuSysfs& operator=(const OneCpuSysfs&) = delete;
  OneCpuSysfs(OneCpuSysfs&&) = delete;
  OneCpuSysfs& operator=(OneCpuSysfs&&) = delete;

  [[nodiscard]] core::Result<topology::Snapshot, topology::DiscoveryFailure> snapshot() const {
    platform::RealSyscalls syscalls;
    platform::FileSystem filesystem(syscalls);
    return topology::take_snapshot(filesystem, root_.string());
  }

 private:
  static void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text << "\n";
  }
  std::filesystem::path root_;
};

struct Rig {
  FakeSyscalls syscalls;
  Choreographer reporter;
  config::Config config;
  RunOptions options;
  void* region_bytes = nullptr;

  Rig() {
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{50};
    config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
    config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
    options.executable = "/opt/loadforge";
    options.interval_ns = 10 * kMs;
    options.stale_after_ns = 100 * kMs;
    options.grace = Grace{20 * kMs, 20 * kMs};
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.script_forks({{kFirstPid, 0}, {kSecondPid, 0}});
  }

  ~Rig() {
    if (region_bytes != nullptr) {
      (void)syscalls.unmap(region_bytes, ipc::HeartbeatRegion::bytes_for(2));
    }
  }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  Rig(Rig&&) = delete;
  Rig& operator=(Rig&&) = delete;

  core::Result<RunReport, ControllerError> run() {
    Controller controller(syscalls, reporter);
    return controller.run(config, options);
  }

  /// The ends a real worker would keep after exec, held from the moment they
  /// exist: called from run_starting, before the controller closes its copies.
  void hold_worker_ends() {
    for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
      syscalls.worker_holds_end(fd);
    }
  }
  void worker_exits(std::size_t slot, int raw_status) {
    syscalls.worker_releases_end(slot == 0 ? kRecordWrite0 : kRecordWrite1);
    syscalls.worker_releases_end(slot == 0 ? kControlRead0 : kControlRead1);
    // Terminated, and a zombie until the reap below delivers it.
    syscalls.child_exits(slot == 0 ? kFirstPid : kSecondPid);
    reaps.push_back(
        FakeSyscalls::WaitStep{Reaped{slot == 0 ? kFirstPid : kSecondPid, raw_status}, 0});
    syscalls.script_nonblocking_reaps(reaps);
  }
  void deliver(std::size_t slot, ipc::RecordType type, std::string_view payload) {
    syscalls.deliver_to_pipe(slot == 0 ? kRecordRead0 : kRecordRead1, frame_text(type, payload));
  }

  /// A beat in a worker's slot, written through a second mapping of the
  /// region's file, so the test never owns the region's descriptor.
  void beat(std::size_t slot, std::uint64_t progress, std::int64_t timestamp_ns,
            std::uint64_t sequence = 2) {
    if (region_bytes == nullptr) {
      auto mapped = syscalls.map_shared(kRegionFd, ipc::HeartbeatRegion::bytes_for(2));
      ASSERT_TRUE(mapped.has_value());
      region_bytes = mapped.value();
    }
    auto* cell = static_cast<std::byte*>(region_bytes) + ipc::HeartbeatRegion::kHeaderBytes +
                 ipc::worker_slot(slot) * ipc::HeartbeatRegion::kSlotBytes;
    const std::uint32_t phase = 1;
    const std::uint32_t pid = 7;
    std::memcpy(cell, &sequence, sizeof sequence);
    std::memcpy(cell + 8, &timestamp_ns, sizeof timestamp_ns);
    std::memcpy(cell + 16, &progress, sizeof progress);
    std::memcpy(cell + 24, &phase, sizeof phase);
    std::memcpy(cell + 28, &pid, sizeof pid);
  }

  std::vector<FakeSyscalls::WaitStep> reaps;
};

bool starts_with(const std::string& line, std::string_view prefix) {
  return line.substr(0, prefix.size()) == prefix;
}

// --- the happy path ----------------------------------------------------------------

TEST(ControllerTest, TwoWorkersRunToTheDeadlineAndAreAskedToStopAndComplete) {
  Rig rig;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 7, 10 * kMs);
      rig.beat(1, 9, 10 * kMs);
    } else if (starts_with(line, "deadline")) {
      // The stop byte is on each control pipe before the event is told, and
      // the controller's own beat has gone into slot 0 once before the fork
      // once in each of the two launches, and on each of the five ticks since:
      // eight beats of two writes each, sequence 16.
      EXPECT_EQ(rig.syscalls.pipe_contents(kControlWrite0), "s") << "the stop byte";
      EXPECT_EQ(rig.syscalls.pipe_contents(kControlWrite1), "s");
      std::uint64_t sequence = 0;
      std::memcpy(&sequence,
                  static_cast<std::byte*>(rig.region_bytes) + ipc::HeartbeatRegion::kHeaderBytes,
                  sizeof sequence);
      EXPECT_EQ(sequence, 16U);
      // And eight beats, counted up from one: the progress a worker reads.
      std::uint64_t progress = 0;
      std::memcpy(
          &progress,
          static_cast<std::byte*>(rig.region_bytes) + ipc::HeartbeatRegion::kHeaderBytes + 16,
          sizeof progress);
      EXPECT_EQ(progress, 8U);
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.deliver(1, ipc::RecordType::kFinished, "done too");
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(rig.syscalls.signals_ignored(), std::vector<int>{SIGPIPE})
      << "a stop byte to a worker that just died must be EPIPE, not signal 13";
  const RunReport& report = result.value();
  EXPECT_FALSE(report.worker_exit_code.has_value());
  ASSERT_EQ(report.workers.size(), 2U);
  EXPECT_EQ(report.workers[0], (WorkerReport{0,
                                             kFirstPid,
                                             WorkerFate::kCompleted,
                                             "exited with code 0",
                                             "stop requested",
                                             7,
                                             {"finished: done"}}));
  EXPECT_EQ(report.workers[1], (WorkerReport{1,
                                             kSecondPid,
                                             WorkerFate::kCompleted,
                                             "exited with code 0",
                                             "stop requested",
                                             9,
                                             {"finished: done too"}}));
  EXPECT_TRUE(all_clean(report));
  EXPECT_EQ(clean_count(report), 2U);
  // Five ticks reach the 50 ms deadline. Both record channels are still open
  // when the last worker is reaped, so the final drain reads the clock twice
  // for each (when it starts, and after the drain that ends it), and the
  // finish reads it once more.
  EXPECT_EQ(report.elapsed_ns, 100 * kMs);
  // Each of the five ticks waited one interval for records; the final drain's
  // polls, after them, wait out the stop grace.
  const std::vector<int>& timeouts = rig.syscalls.poll_timeouts();
  ASSERT_GE(timeouts.size(), 5U);
  EXPECT_EQ(std::vector<int>(timeouts.begin(), timeouts.begin() + 5),
            (std::vector<int>{10, 10, 10, 10, 10}));
  EXPECT_EQ(
      rig.reporter.events,
      (std::vector<std::string>{
          "spawned 2 worker(s): pids 1001 1002", "deadline reached: stop requested",
          "worker 0 (pid 1001): exited with code 0", "worker 1 (pid 1002): exited with code 0"}));
  EXPECT_EQ(rig.reporter.elapsed, (std::vector<std::int64_t>{0, 50 * kMs, 50 * kMs, 50 * kMs}));
  // Nobody needed a signal. Each worker's process group was swept as it was
  // reaped, and was empty, so the sweep left no note.
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{{-kFirstPid, SIGKILL}, {-kSecondPid, SIGKILL}}));
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  ASSERT_EQ(rig.reporter.plans.size(), 1U);
  EXPECT_EQ(rig.reporter.plans[0].workers, 2U);
  EXPECT_EQ(rig.reporter.plans[0].executable, "/opt/loadforge");
  EXPECT_EQ(rig.reporter.plans[0].limits.size(), 1U);
  EXPECT_EQ(rig.reporter.plans[0].topology, "no snapshot offered");
  ASSERT_EQ(rig.reporter.reports.size(), 1U);
  EXPECT_EQ(rig.reporter.reports[0], report);
}

TEST(ControllerTest, ThePlanDescribesTheSnapshotItWasGivenBeforeAnyWorkerExists) {
  // The snapshot is taken by the caller before the run (docs/PLAN.md §4.1);
  // the controller carries it into the plan, which reaches the reporter
  // before the first fork.
  const OneCpuSysfs tree;
  auto snapshot = tree.snapshot();
  ASSERT_TRUE(snapshot.has_value()) << topology::describe(snapshot.error());
  Rig rig;
  rig.options.topology = &snapshot.value();
  int forks_at_start = -1;
  rig.reporter.on_starting = [&rig, &forks_at_start] {
    forks_at_start = rig.syscalls.fork_count();
    rig.hold_worker_ends();
  };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(rig.reporter.plans.size(), 1U);
  EXPECT_EQ(rig.reporter.plans[0].topology,
            "1 logical CPU(s) on 1 core(s) in 1 package(s); caches not exposed; NUMA not exposed");
  EXPECT_EQ(forks_at_start, 0) << "the plan, and the snapshot in it, came before any fork";
}

TEST(ControllerTest, TheChildOfAFailedExecReturnsItsOwnExitCodeAndTouchesNothingElse) {
  Rig rig;
  rig.syscalls.script_forks({{0, 0}});  // This process is the first worker.
  // In the child, getppid is the controller's pid; the fake's parent must be
  // what its own process_id says, or the launcher sees the orphan race.
  rig.syscalls.set_parent_pid(FakeSyscalls::kFakePid);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().worker_exit_code.has_value());
  EXPECT_EQ(*result.value().worker_exit_code, 120) << "exec failed: the launcher's code";
  EXPECT_TRUE(result.value().workers.empty());
  // Exactly its own descriptors were handed over, and the argv is the one the
  // worker's parser reads back.
  // Read from the history: the child's teardown closed its copies after the
  // exec failed, as it should, so the live view is empty again.
  EXPECT_EQ(rig.syscalls.handed_over(),
            (std::vector<int>{kRegionFd, kRecordWrite0, kControlRead0}));
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  worker::WorkerArgs expected;
  expected.slot = 0;
  expected.workers = 2;
  expected.heartbeat_fd = kRegionFd;
  expected.record_fd = kRecordWrite0;
  expected.control_fd = kControlRead0;
  expected.supervisor_pid = FakeSyscalls::kFakePid;
  expected.pacing = worker::Pacing{50 * kMs, 10 * kMs, 100 * kMs};
  EXPECT_EQ(rig.syscalls.last_exec_argv(), worker::argv_for("/opt/loadforge", expected));
  EXPECT_EQ(rig.syscalls.last_exec_path(), "/opt/loadforge");
  EXPECT_TRUE(rig.reporter.events.empty()) << "a child reports nothing";
  EXPECT_TRUE(rig.reporter.reports.empty());
}

// --- the catalogue ---------------------------------------------------------------

TEST(ControllerTest, AWorkerWhoseSignalsAreRefusedAsGoneIsNeitherCountedSignalledNorBlamed) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  // Worker 0 beat once, stamped with the last launch: stale by timestamp once
  // the allowance passes. Worker 1 never beat: stale once the allowance has
  // passed since its launch, which is that same instant. Both on one tick.
  // Worker 1's signals are refused with ESRCH, which the
  // kernel gives only for a child already reaped: tolerated, but no signal
  // was sent, so none is recorded, and the worker is signalled again on the
  // next tick until the reap says how it went.
  std::int64_t last_launch_ns = 0;
  rig.syscalls.on_fork_returned([&rig, &last_launch_ns](int) {
    last_launch_ns = rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC);
  });
  rig.reporter.on_event = [&rig, &last_launch_ns](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, last_launch_ns);
    } else if (line == "worker 0: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  rig.syscalls.fail_signal_for(kSecondPid, ESRCH);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(result.value().workers[0].termination, "killed by signal 9");
  EXPECT_EQ(result.value().workers[0].stop, "SIGKILL sent");
  // Worker 1 was never successfully signalled: its death is a kill nobody
  // here is shown to have caused.
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kKilled);
  EXPECT_EQ(result.value().workers[1].stop, "running");
  for (const std::string& event : rig.reporter.events) {
    EXPECT_FALSE(starts_with(event, "worker 1: sent signal")) << event;
  }
  EXPECT_FALSE(all_clean(result.value()));
}

TEST(ControllerTest, AWorkerWhoseBeatGoesStaleIsTerminatedThenKilledAndTimedOut) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, 0);
    } else if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  for (const WorkerReport& worker : result.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kTimedOut) << worker.slot;
    EXPECT_EQ(worker.termination, "killed by signal 9");
    EXPECT_EQ(worker.stop, "SIGKILL sent");
  }
  // Each escalation looked first: SIGTERM and SIGKILL for two workers.
  EXPECT_EQ(rig.syscalls.exit_peeks(),
            (std::vector<pid_t>{kFirstPid, kFirstPid, kSecondPid, kSecondPid}));
  // Neither worker beats, and each is stale 30 ms after its own launch; the
  // first was launched first, so it is told first (its SIGTERM at 20 ms from
  // the last launch) and its SIGKILL comes a 20 ms grace later, before the
  // second has been told anything.
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{{kFirstPid, SIGTERM},
                                                {kFirstPid, SIGKILL},
                                                {kSecondPid, SIGTERM},
                                                {kSecondPid, SIGKILL},
                                                {-kFirstPid, SIGKILL},
                                                {-kSecondPid, SIGKILL}}));
  EXPECT_EQ(rig.reporter.events[1], "worker 0: sent signal 15 (heartbeat stale)");
  EXPECT_EQ(rig.reporter.elapsed[1], 20 * kMs);
  EXPECT_EQ(rig.reporter.elapsed[3], 40 * kMs);
  EXPECT_FALSE(all_clean(result.value()));
}

// --- startup is its own supervised phase (EXT-02) ---------------------------------
//
// Every fork takes 60 ms of the fake clock against a 100 ms allowance: two
// forks are 120 ms, longer than any worker may wait for the controller's beat.

constexpr std::int64_t kForkDelay = 60 * kMs;

/// The raw and running readings the next clock read will give, taken at the
/// fork boundary: the controller's next read is its record of the launch.
struct Launch {
  std::int64_t raw_ns = 0;
  std::int64_t running_ns = 0;
};

TEST(ControllerStartupTest, AWorkerAlreadyRunningNeverFindsTheControllersBeatStaleWhileItForks) {
  Rig rig;
  rig.syscalls.delay_each_fork(kForkDelay);
  struct Look {
    int fork = 0;
    ipc::Liveness liveness = ipc::Liveness::kNeverBeat;
  };
  std::vector<Look> looks;
  rig.syscalls.on_fork_returned([&rig, &looks](int index) {
    if (rig.region_bytes == nullptr) {
      auto mapped = rig.syscalls.map_shared(kRegionFd, ipc::HeartbeatRegion::bytes_for(2));
      ASSERT_TRUE(mapped.has_value());
      rig.region_bytes = mapped.value();
    }
    // What a worker started by fork 0 would read of slot 0 at this instant.
    std::int64_t timestamp = 0;
    std::memcpy(&timestamp,
                static_cast<std::byte*>(rig.region_bytes) + ipc::HeartbeatRegion::kHeaderBytes + 8,
                sizeof timestamp);
    const ipc::Heartbeat beat{timestamp, 1, 1, 7};
    looks.push_back(Look{index, ipc::liveness(beat, rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC),
                                              rig.options.stale_after_ns)});
  });
  rig.config.run.duration = core::Duration{20};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC));
      rig.beat(1, 1, rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC));
    } else if (starts_with(line, "deadline")) {
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(looks.size(), 2U);
  for (const Look& look : looks) {
    EXPECT_NE(look.liveness, ipc::Liveness::kStale)
        << "after fork " << look.fork << " the beat a running worker reads is already stale";
  }
}

TEST(ControllerStartupTest, AWorkerForkedLateIsGivenItsAllowanceFromItsOwnLaunch) {
  Rig rig;
  rig.syscalls.delay_each_fork(kForkDelay);
  rig.config.run.duration = core::Duration{1000};
  std::vector<Launch> launches;
  rig.syscalls.on_fork_returned([&rig, &launches](int) {
    launches.push_back(Launch{rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC_RAW),
                              rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC)});
  });
  // The raw clock when each worker was first told to stop for staleness. The
  // event follows the tick's own read of the clock, so the next read would be
  // one step (10 ms) further on.
  std::map<std::size_t, std::int64_t> told_at;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig, &told_at](const std::string& line) {
    for (std::size_t slot = 0; slot < 2; ++slot) {
      if (line == "worker " + std::to_string(slot) + ": sent signal 15 (heartbeat stale)") {
        told_at[slot] = rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC_RAW) - 10 * kMs;
      }
    }
    if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(launches.size(), 2U);
  // Neither worker ever beats. Each is judged stale only once the allowance
  // has passed since ITS launch, and both are judged in the end.
  ASSERT_EQ(told_at.size(), 2U);
  for (std::size_t slot = 0; slot < 2; ++slot) {
    EXPECT_GT(told_at[slot] - launches[slot].raw_ns, rig.options.stale_after_ns)
        << "worker " << slot << " was judged stale before its own allowance had passed";
  }
}

TEST(ControllerStartupTest, TheDurationCountsFromTheLastLaunchNotFromPreparation) {
  Rig rig;
  rig.syscalls.delay_each_fork(kForkDelay);
  std::int64_t last_launch_ns = 0;
  rig.syscalls.on_fork_returned([&rig, &last_launch_ns](int) {
    last_launch_ns = rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC);
  });
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig, &last_launch_ns](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, last_launch_ns);
      rig.beat(1, 1, last_launch_ns);
    } else if (starts_with(line, "deadline")) {
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_GE(rig.reporter.events.size(), 2U);
  EXPECT_EQ(rig.reporter.events[0], "spawned 2 worker(s): pids 1001 1002");
  EXPECT_EQ(rig.reporter.elapsed[0], 0) << "the run begins when the last worker is launched";
  EXPECT_EQ(rig.reporter.events[1], "deadline reached: stop requested");
  EXPECT_EQ(rig.reporter.elapsed[1], 50 * kMs)
      << "the whole configured duration, after the 120 ms the forks took";
}

TEST(ControllerStartupTest, AClockThatFailsBetweenForksIsTheControllersFailureAndStopsTheSpawn) {
  Rig rig;
  // Reads of the raw clock: preparation, then one per launch. The second
  // fails, with the first worker already running.
  rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC_RAW, 3, EIO);
  rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}, {Reaped{kSecondPid, SIGKILL}, 0}});
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "clock");
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{{kFirstPid, SIGKILL}, {kSecondPid, SIGKILL}}))
      << "both workers had been forked, and the pool unwound them";
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerTest, AWorkerThatBeatsWithoutProgressIsASpinAndTimesOut) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      // Beats from far in the future stay fresh by timestamp forever; the
      // progress never moves, which is the spin.
      rig.beat(0, 7, 10'000 * kMs);
      rig.beat(1, 7, 10'000 * kMs);
    } else if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(result.value().workers[0].progress, 7U);
  // Progress 7 was seen at tick 1 (10 ms); 40 ms later with no change, at
  // tick 5, the allowance of 30 ms is exceeded.
  EXPECT_EQ(rig.reporter.elapsed[1], 50 * kMs);
}

TEST(ControllerTest, AWorkerThatDiedInsideAPublishIsTreatedAsStale) {
  // Once on a clock that starts at zero, and once on one that starts where a
  // real machine's does, well past it: the allowance is measured from the
  // start's reading, and an event's time is the time since it, so neither
  // may depend on the reading itself.
  for (const std::int64_t base_s : {std::int64_t{0}, std::int64_t{86'400}}) {
    Rig rig;
    for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
      rig.syscalls.set_clock(clock_id, platform::TimeSpec{base_s, 0});
    }
    const std::int64_t base_ns = base_s * 1000 * kMs;
    rig.config.run.duration = core::Duration{1000};
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.reporter.on_event = [&rig, base_ns](const std::string& line) {
      if (starts_with(line, "spawned")) {
        rig.beat(0, 1, base_ns + 10 * kMs, 3);  // An odd sequence: a writer that never finished.
        rig.beat(1, 1, base_ns + 10'000 * kMs);
      } else if (line == "worker 0: sent signal 15 (heartbeat stale)") {
        rig.worker_exits(0, SIGSEGV);
      } else if (line == "worker 0 (pid 1001): killed by signal 11") {
        rig.worker_exits(1, 0);
      }
    };
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << describe(result.error());
    EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kFault)
        << "SIGTERM was sent, but what killed it was the fault";
    // Unreadable from the start, and stale once that has lasted longer than
    // the 100 ms allowance: the tick at 100 ms from the last launch, not the first one.
    ASSERT_GE(rig.reporter.events.size(), 2U) << base_s;
    EXPECT_EQ(rig.reporter.events[1], "worker 0: sent signal 15 (heartbeat stale)") << base_s;
    EXPECT_EQ(rig.reporter.elapsed[1], 100 * kMs) << "unreadable past the allowance, " << base_s;
  }
}

TEST(ControllerTest, ASlotUnreadableForLessThanTheAllowanceIsNotADeath) {
  // A worker descheduled between the two halves of a publish leaves its slot
  // unreadable for as long as the scheduler likes. The first version called
  // one torn read a death, and under TSan with every core busy it SIGTERMed a
  // healthy worker in a 600 ms run. Torn from the start of a 50 ms run with a
  // 100 ms allowance, worker 0 is never signalled, and completes.
  Rig rig;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, 10 * kMs, 3);
      rig.beat(1, 1, 10'000 * kMs);
    } else if (starts_with(line, "deadline")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
  EXPECT_EQ(result.value().workers[0].stop, "stop requested");
  for (const auto& [pid, signal] : rig.syscalls.signals_sent()) {
    EXPECT_LT(pid, 0) << "only the group sweeps; signal " << signal << " to " << pid;
  }
}

TEST(ControllerTest, AStopRequestIgnoredIsEscalatedAndOneWorkerHonoursTheTerm) {
  Rig rig;
  rig.config.run.duration = core::Duration{30};
  rig.options.stale_after_ns = 1000 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, 10'000 * kMs);
      rig.beat(1, 1, 10'000 * kMs);
    } else if (line == "worker 0: sent signal 15") {
      rig.deliver(0, ipc::RecordType::kFinished, "stopped on term");
      rig.worker_exits(0, 0);
    } else if (line == "worker 1: sent signal 9") {
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
  EXPECT_EQ(result.value().workers[0].stop, "SIGTERM sent");
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kStoppedByController);
  EXPECT_EQ(result.value().workers[1].stop, "SIGKILL sent");
  // Deadline at tick 3 (30 ms), SIGTERM 20 ms later, SIGKILL 20 ms after that.
  EXPECT_EQ(rig.reporter.events[1], "deadline reached: stop requested");
  EXPECT_EQ(rig.reporter.elapsed[1], 30 * kMs);
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{{kFirstPid, SIGTERM},
                                                {kSecondPid, SIGTERM},
                                                {-kFirstPid, SIGKILL},
                                                {kSecondPid, SIGKILL},
                                                {-kSecondPid, SIGKILL}}));
}

TEST(ControllerTest, AFaultIsReportedWithTheRecordsThatArrivedBeforeIt) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kStarted, "slot 0 pid 1001");
      rig.deliver(0, ipc::RecordType::kError, "about to crash");
      rig.worker_exits(0, SIGSEGV);
      rig.syscalls.deliver_to_pipe(kRecordRead1,
                                   frame_text(static_cast<ipc::RecordType>(900), "?"));
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[1].records,
            (std::vector<std::string>{"unknown record: ?", "finished: done"}))
      << "a type this build does not know is carried, not refused";
  EXPECT_EQ(result.value().workers[0],
            (WorkerReport{0,
                          kFirstPid,
                          WorkerFate::kFault,
                          "killed by signal 11",
                          "running",
                          0,
                          {"started: slot 0 pid 1001", "error: about to crash"}}));
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kCompleted);
  EXPECT_EQ(clean_count(result.value()), 1U);
}

TEST(ControllerTest, WhatAWorkerLeftRunningInItsGroupIsKilledAndRecorded) {
  // The catalogue's "fork a grandchild, then die": worker 1's group still has
  // a member when it is reaped, so the sweep kills it and says so; worker 0's
  // group is empty and its record is untouched. The real processes are the
  // supervision tier's (tests/supervision/catalogue_test.cpp).
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.syscalls.group_left_running(kSecondPid);
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      rig.deliver(1, ipc::RecordType::kError, "forked and left");
      rig.worker_exits(1, 1 << 8);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].records, (std::vector<std::string>{"finished: done"}));
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kWorkerError);
  EXPECT_EQ(result.value().workers[1].records,
            (std::vector<std::string>{"error: forked and left",
                                      "process group: killed what the worker left running"}))
      << "the sweep is recorded when the worker is reaped, after what it sent";
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{{-kFirstPid, SIGKILL}, {-kSecondPid, SIGKILL}}));
}

TEST(ControllerTest, AGroupThatCannotBeKilledIsRecordedAndTheRunGoesOn) {
  // Any refusal but ESRCH means something may still be running that the
  // controller could not stop: a fact about the worker, not a failure of the
  // run, so it goes in the worker's record.
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.syscalls.fail_signal_for(-kFirstPid, EPERM);
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(result.value().workers[0].records.size(), 2U);
  EXPECT_EQ(result.value().workers[0].records[0], "finished: done");
  EXPECT_EQ(result.value().workers[0].records[1].substr(0, 15), "process group: ");
  EXPECT_NE(result.value().workers[0].records[1].find("kill"), std::string::npos)
      << result.value().workers[0].records[1];
  EXPECT_EQ(result.value().workers[1].records, (std::vector<std::string>{"finished: done"}));
}

TEST(ControllerTest, ATeardownSweepsOnlyTheGroupsOfWorkersNotYetReaped) {
  // Worker 0 is reaped (and its group swept) on the first tick; the second
  // tick's poll fails. The teardown sweeps worker 1's group, whose pid still
  // pins it, and not worker 0's again, whose number may since belong to
  // somebody else.
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
    } else if (starts_with(line, "worker 0 (pid 1001)")) {
      rig.syscalls.fail_poll(EINVAL);
    }
  };
  rig.syscalls.script_waits({{Reaped{kSecondPid, SIGKILL}, 0}});
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "record channel");
  EXPECT_EQ(rig.syscalls.signals_sent(),
            (std::vector<std::pair<pid_t, int>>{
                {-kFirstPid, SIGKILL}, {-kSecondPid, SIGKILL}, {kSecondPid, SIGKILL}}));
  EXPECT_EQ(rig.syscalls.wait_count(), 1)
      << "one worker was left, so one wait: none fewer, and none more once it was reaped";
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerTest, OnceTheLastWorkerIsReapedTheKernelIsAskedNothingMore) {
  // With no children left, a non-blocking waitpid answers ECHILD, which in
  // the loop is the controller's own failure. Both workers are reaped on the
  // first tick, and the kernel's next answer is scripted as that ECHILD: the
  // run completes only if nothing asks for it.
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
      rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{}, ECHILD});
      rig.syscalls.script_nonblocking_reaps(rig.reaps);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(rig.syscalls.nonblocking_reap_count(), 2) << "one reap per worker, and no more";
  EXPECT_EQ(rig.syscalls.wait_count(), 0) << "and the teardown had nobody to wait for";
  EXPECT_TRUE(all_clean(result.value()));
}

// --- the OOM killer, from the kernel's record ---------------------------------------

/// Worker 0 dies of a SIGKILL nobody here sent; worker 1 completes.
void one_killed_one_completes(Rig& rig) {
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kStarted, "slot 0 pid 1001");
      rig.worker_exits(0, SIGKILL);
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(1, 0);
    }
  };
}

TEST(ControllerOomTest, ASigkillTheCountAccountsForIsAnOomKillAndTheOtherWorkerCompletes) {
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{3}, std::uint64_t{4}};
  rig.options.oom_record = &record;
  one_killed_one_completes(rig);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0],
            (WorkerReport{0,
                          kFirstPid,
                          WorkerFate::kOomKilled,
                          "killed by signal 9",
                          "running",
                          0,
                          {"started: slot 0 pid 1001",
                           "OOM kill: the scripted count counted 1 during the run, one or more for "
                           "each of the 1 worker(s) killed by a SIGKILL"}}));
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kCompleted);
  EXPECT_EQ(result.value().workers[1].records, (std::vector<std::string>{"finished: done"}))
      << "a worker the count does not concern carries no word about it";
  EXPECT_EQ(record.reads, 2) << "once before the first fork, once after the last reap";
  EXPECT_FALSE(all_clean(result.value())) << "an OOM kill is not a completion";
}

TEST(ControllerOomTest, TheCountIsReadBeforeTheFirstFork) {
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}};
  rig.options.oom_record = &record;
  rig.reporter.on_starting = [&rig, &record] {
    EXPECT_EQ(record.reads, 1) << "read before the run was announced";
    EXPECT_EQ(rig.syscalls.fork_count(), 0);
    rig.hold_worker_ends();
  };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "deadline")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.deliver(1, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_TRUE(all_clean(result.value()));
  EXPECT_EQ(record.reads, 1) << "nobody died of SIGKILL, so the count is not read again";
}

TEST(ControllerOomTest, ASigkillTheCountDoesNotAccountForStaysAKillAndSaysWhy) {
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{3}, std::uint64_t{3}};
  rig.options.oom_record = &record;
  one_killed_one_completes(rig);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kKilled);
  EXPECT_EQ(result.value().workers[0].records.back(),
            "not attributed to the OOM killer: the scripted count counted no OOM kill during the "
            "run");
}

TEST(ControllerOomTest, ACountUnreadableAtEitherEndLeavesAKillWithTheReason) {
  {
    Rig rig;
    ScriptedOomRecord record;
    record.readings = {std::string{"no count before"}, std::uint64_t{9}};
    rig.options.oom_record = &record;
    one_killed_one_completes(rig);
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << describe(result.error());
    EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kKilled)
        << "a count the run could not read first is no reason to refuse the run";
    EXPECT_EQ(result.value().workers[0].records.back(),
              "not attributed to the OOM killer: the OOM-kill count could not be read before the "
              "run: no count before");
  }
  {
    Rig rig;
    ScriptedOomRecord record;
    record.readings = {std::uint64_t{0}, std::string{"no count after"}};
    rig.options.oom_record = &record;
    one_killed_one_completes(rig);
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << describe(result.error());
    EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kKilled);
    EXPECT_EQ(result.value().workers[0].records.back(),
              "not attributed to the OOM killer: the OOM-kill count could not be read after the "
              "run: no count after");
  }
}

TEST(ControllerOomTest, TwoSigkillsAndOneCountedKillAttributeNeither) {
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}, std::uint64_t{1}};
  rig.options.oom_record = &record;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.worker_exits(0, SIGKILL);
    } else if (starts_with(line, "worker 0 (pid")) {
      rig.worker_exits(1, SIGKILL);  // Reaped after worker 0's death was told.
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const std::string note =
      "not attributed to the OOM killer: the scripted count counted 1 OOM kill(s) during the run "
      "for 2 workers killed by a SIGKILL, and a count does not say "
      "which";
  for (const WorkerReport& worker : result.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kKilled) << worker.slot;
    EXPECT_EQ(worker.records, (std::vector<std::string>{note})) << worker.slot;
  }
  EXPECT_EQ(record.reads, 2);
}

/// Both workers stale and both escalated to SIGKILL by the controller on the
/// same tick; `before_second` runs between the two escalations, which is the
/// one place a test can act inside a tick (the first escalation's event).
void both_escalated(Rig& rig, ScriptedOomRecord& record,
                    const std::function<void()>& before_second) {
  rig.options.oom_record = &record;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig, before_second](const std::string& line) {
    if (line == "worker 0: sent signal 9 (heartbeat stale)") {
      before_second();
    } else if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(1, SIGKILL);
    }
  };
}

TEST(ControllerOomTest, TheControllersOwnSigkillIsAskedAboutAndStaysItsStopWhenTheCountDidNotMove) {
  // An attempted SIGKILL proves nothing about the cause (kill succeeds on a
  // zombie), so the count is read; unmoved, the attempt stands.
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}, std::uint64_t{0}};
  both_escalated(rig, record, [&rig] { rig.worker_exits(0, SIGKILL); });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(record.reads, 2) << "read again after the last reap: an attempt is not a cause";
  EXPECT_EQ(result.value().workers[1].records.back(),
            "not attributed to the OOM killer: the scripted count counted no OOM kill during the "
            "run");
}

TEST(ControllerOomTest, ARiseInTheCountOutranksTheControllersAttemptedSigkill) {
  // The worker died of SIGKILL in the instant between the controller's look
  // and its kill (the look said running: it is not a zombie yet in the fake),
  // and the count rose by one: the OOM killer, not the controller.
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{5}, std::uint64_t{6}};
  rig.options.oom_record = &record;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      // Worker 0 beats on, from far in the future, and never becomes stale.
      rig.beat(0, 7, 100'000 * kMs);
    } else if (line == "worker 1: sent signal 15 (heartbeat stale)") {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
    } else if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      // Dead of SIGKILL, but unseen by the look that came before the kill.
      rig.syscalls.worker_releases_end(kRecordWrite1);
      rig.syscalls.worker_releases_end(kControlRead1);
      rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, SIGKILL}, 0});
      rig.syscalls.script_nonblocking_reaps(rig.reaps);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
  const WorkerReport& killed = result.value().workers[1];
  EXPECT_EQ(killed.stop, "SIGKILL sent");
  EXPECT_EQ(killed.fate, WorkerFate::kOomKilled) << describe(killed.fate);
  EXPECT_EQ(record.reads, 2);
}

TEST(ControllerOomTest, ACountThatCannotSpeakLeavesTheControllersSigkillAmbiguous) {
  // Two workers dead of the controller's attempted SIGKILL and one OOM kill
  // counted: a count does not say which. Neither is claimed as the
  // controller's stop nor as an OOM kill.
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}, std::uint64_t{1}};
  both_escalated(rig, record, [&rig] { rig.worker_exits(0, SIGKILL); });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  for (const WorkerReport& worker : result.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kKilled) << worker.slot;
    EXPECT_EQ(worker.stop, "SIGKILL sent") << "the attempt stays in the report";
  }
  const std::vector<std::string>& notes = result.value().workers[0].records;
  ASSERT_FALSE(notes.empty());
  EXPECT_EQ(notes.back(),
            "not attributed to the OOM killer: the scripted count counted 1 OOM kill(s) during "
            "the run for 2 workers killed by a SIGKILL, and a count does not say which");
}

TEST(ControllerOomTest, ACountUnreadableLeavesTheControllersSigkillAmbiguousToo) {
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}, std::string{"no count after"}};
  both_escalated(rig, record, [&rig] { rig.worker_exits(0, SIGKILL); });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  for (const WorkerReport& worker : result.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kKilled) << worker.slot;
  }
}

TEST(ControllerOomTest, WithNoRecordAnAttemptedSigkillIsTheControllersStop) {
  // Nothing consulted: nothing to contradict the attempt, as before.
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTimedOut);
}

// --- EXT-01: a worker already dead is never signalled ---------------------------------

TEST(ControllerExitedTest, AWorkerKilledFromOutsideBeforeItsEscalationIsNeverSignalledNorBlamed) {
  // Both workers are due SIGKILL on the same tick. After the first is sent
  // (the controller's own kill of a running worker), the second is killed
  // from outside and is a zombie when its turn comes. Before the fix the
  // controller's kill of the zombie succeeded, was recorded, and the report
  // called the outside kill its own timeout.
  Rig rig;
  ScriptedOomRecord record;
  record.readings = {std::uint64_t{0}, std::uint64_t{0}};
  both_escalated(rig, record, [&rig] {
    rig.worker_exits(0, SIGKILL);  // The controller's kill landing.
    rig.worker_exits(1, SIGKILL);  // An outside SIGKILL, unreaped.
  });
  // Each worker's allowance runs from its own launch, so a worker that never
  // beat would be due a tick before the one forked after it. One beat stamped
  // with the second launch makes both due on the same tick, as this test needs.
  std::int64_t second_launch_ns = 0;
  rig.syscalls.on_fork_returned([&rig, &second_launch_ns](int) {
    second_launch_ns = rig.syscalls.peek_clock_ns(CLOCK_MONOTONIC);
  });
  const auto escalate = rig.reporter.on_event;
  rig.reporter.on_event = [&rig, &second_launch_ns, escalate](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.beat(0, 1, second_launch_ns);
    }
    escalate(line);
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const WorkerReport& controllers = result.value().workers[0];
  const WorkerReport& outside = result.value().workers[1];
  EXPECT_EQ(controllers.fate, WorkerFate::kTimedOut) << "its own kill of a running worker";
  EXPECT_EQ(outside.fate, WorkerFate::kKilled) << describe(outside.fate);
  EXPECT_EQ(outside.stop, "SIGTERM sent") << "no SIGKILL was sent to a worker already dead";
  const std::vector<std::pair<pid_t, int>> signals = rig.syscalls.signals_sent();
  EXPECT_EQ(std::ranges::count(signals, std::pair<pid_t, int>{kSecondPid, SIGKILL}), 0)
      << "never signal a worker known dead";
  EXPECT_EQ(rig.reporter.events.end(),
            std::ranges::find(rig.reporter.events, "worker 1: sent signal 9 (heartbeat stale)"));
  EXPECT_EQ(record.reads, 2) << "the OOM count is consulted";
  EXPECT_EQ(outside.records.front(), "signal 9 not sent: the worker had already exited");
  EXPECT_EQ(outside.records.back(),
            "not attributed to the OOM killer: the scripted count counted no OOM kill during the "
            "run");
}

TEST(ControllerExitedTest, ARefusedLookIsAControllerErrorAndNothingIsSent) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.syscalls.fail_has_exited(EINVAL);
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "signal");
  EXPECT_NE(result.error().detail.find("waitid"), std::string::npos) << result.error().detail;
  for (const auto& [pid, signal] : rig.syscalls.signals_sent()) {
    EXPECT_NE(signal, SIGTERM) << pid;
  }
}

TEST(ControllerOomTest, WithNoRecordOfferedASigkillIsAKillAndNothingIsSaidAboutOom) {
  Rig rig;
  one_killed_one_completes(rig);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kKilled);
  EXPECT_EQ(result.value().workers[0].records,
            (std::vector<std::string>{"started: slot 0 pid 1001"}));
}

TEST(ControllerOomTest, ABudgetThatCouldNotBeReadIsNotEnforcedAndTheBannerSaysWhy) {
  Rig rig;
  rig.config.limits.memory = core::ByteSize::bytes(std::uint64_t{1} << 30U);
  rig.options.budget_unread = "/proc/meminfo: not permitted (open: Permission denied)";
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.worker_exits(0, 0);
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(rig.reporter.plans.size(), 1U);
  ASSERT_EQ(rig.reporter.plans[0].limits.size(), 2U);
  EXPECT_EQ(rig.reporter.plans[0].limits[1],
            (LimitStatus{"memory", false,
                         "no memory budget could be read: /proc/meminfo: not permitted (open: "
                         "Permission denied)"}));
}

TEST(ControllerTest, ACorruptFrameAndATruncatedStreamAreEachReportedAsSuch) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      std::string corrupt = frame_text(ipc::RecordType::kFinished, "done");
      corrupt[4] = '\x02';  // version 2
      rig.syscalls.deliver_to_pipe(kRecordRead0, corrupt);
      rig.worker_exits(0, 0);
      const std::string whole = frame_text(ipc::RecordType::kFinished, "cut");
      rig.syscalls.deliver_to_pipe(kRecordRead1, whole.substr(0, whole.size() - 1));
      rig.worker_exits(1, 0);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kChannelCorrupt);
  EXPECT_EQ(result.value().workers[0].records,
            (std::vector<std::string>{"record channel: the frame is of a version this build "
                                      "does not read: version 2, expected 1"}));
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTruncatedEvidence);
  EXPECT_EQ(result.value().workers[1].records,
            (std::vector<std::string>{
                "record channel: the stream ended inside a frame: bytes left 18, expected 0"}));
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerTest, AChannelHeldOpenAfterTheWorkerDiedIsTruncatedNotWaitedForForever) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      // Worker 1 dies, but something it started still holds its record pipe's
      // write end: only the control end is released.
      rig.syscalls.worker_releases_end(kControlRead1);
      rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
      rig.syscalls.script_nonblocking_reaps(rig.reaps);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTruncatedEvidence);
  EXPECT_EQ(result.value().workers[1].records,
            (std::vector<std::string>{"record channel: held open after the worker died"}));
  // The final drain waited one stop grace on the held channel, then gave up.
  EXPECT_EQ(rig.syscalls.poll_timeouts().back(), 20);
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerTest, AChannelThatNeverEndsIsGivenABoundedNumberOfDrains) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  // A stop grace far longer than 1024 drains take on the fake clock, so the
  // count is the bound that stops it; the time bound has its own test below.
  rig.options.grace.stop_ns = std::int64_t{3600} * 1000 * kMs;
  int reads_when_last_reaped = 0;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig, &reads_when_last_reaped](const std::string& line) {
    // The last event of the run is the last reap; every read after it belongs
    // to the final drain, which is what the bound governs.
    reads_when_last_reaped = rig.syscalls.reads_of(kRecordRead1);
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      // Most of a large frame, a byte per read, from a writer that stays:
      // more bytes than the bound allows drains, so only the bound stops it.
      const std::string big = frame_text(ipc::RecordType::kFinished, std::string(2000, 'x'));
      rig.syscalls.deliver_to_pipe(kRecordRead1, big.substr(0, 1500));
      rig.syscalls.limit_pipe_reads(1);
      rig.syscalls.worker_releases_end(kControlRead1);
      rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
      rig.syscalls.script_nonblocking_reaps(rig.reaps);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTruncatedEvidence);
  EXPECT_EQ(result.value().workers[1].records.back(),
            "record channel: held open after the worker died");
  // Exactly the bound: 1024 drains of one byte each, with bytes still waiting.
  // A bound that only had to be "more than 1000" let a bound a thousand times
  // larger pass, which is the same as no bound at all.
  EXPECT_EQ(rig.syscalls.reads_of(kRecordRead1) - reads_when_last_reaped, 1024)
      << "the bound, not the stream's end, stopped it";
}

TEST(ControllerTest, AChannelThatTricklesIsGivenOneStopGraceInAllNotOnePerDrain) {
  Rig rig;
  rig.config.run.duration = core::Duration{1000};
  int reads_when_last_reaped = 0;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig, &reads_when_last_reaped](const std::string& line) {
    reads_when_last_reaped = rig.syscalls.reads_of(kRecordRead1);
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done");
      rig.worker_exits(0, 0);
      // Bytes always waiting, a byte per read, from a writer that stays: a
      // channel that would never end by itself and never goes quiet.
      const std::string big = frame_text(ipc::RecordType::kFinished, std::string(2000, 'x'));
      rig.syscalls.deliver_to_pipe(kRecordRead1, big.substr(0, 1500));
      rig.syscalls.limit_pipe_reads(1);
      rig.syscalls.worker_releases_end(kControlRead1);
      rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
      rig.syscalls.script_nonblocking_reaps(rig.reaps);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTruncatedEvidence);
  // The stop grace is 20 ms and the fake clock moves 10 ms per reading: the
  // first poll may wait the whole grace, the second only what is left of it,
  // and then the grace is spent. Two drains, not 1024.
  EXPECT_EQ(rig.syscalls.reads_of(kRecordRead1) - reads_when_last_reaped, 2);
  const std::vector<int>& timeouts = rig.syscalls.poll_timeouts();
  ASSERT_GE(timeouts.size(), 2U);
  EXPECT_EQ(timeouts[timeouts.size() - 2], 20);
  EXPECT_EQ(timeouts.back(), 10);
}

TEST(ControllerTest, AClockThatFailsDuringTheFinalDrainIsTheControllersFailure) {
  // The final drain reads the clock when it starts on a held channel and after
  // each drain; either reading failing ends the run as the controller's own
  // failure, after the workers are gone.
  for (const int extra : {1, 2}) {
    Rig rig;
    rig.config.run.duration = core::Duration{1000};
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.reporter.on_event = [&rig, extra](const std::string& line) {
      if (starts_with(line, "worker ")) {
        // A reap, and the last event before the final drain is the last reap:
        // the next raw reading is the drain's first, the one after its second.
        const auto raw_reads =
            static_cast<int>(std::count(rig.syscalls.clocks_read().begin(),
                                        rig.syscalls.clocks_read().end(), CLOCK_MONOTONIC_RAW));
        rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC_RAW, raw_reads + extra, EIO);
      }
      if (starts_with(line, "spawned")) {
        rig.deliver(0, ipc::RecordType::kFinished, "done");
        rig.worker_exits(0, 0);
        rig.syscalls.deliver_to_pipe(kRecordRead1, "xx");
        rig.syscalls.limit_pipe_reads(1);
        rig.syscalls.worker_releases_end(kControlRead1);
        rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
        rig.syscalls.script_nonblocking_reaps(rig.reaps);
      }
    };
    auto result = rig.run();
    ASSERT_FALSE(result.has_value()) << extra;
    EXPECT_EQ(result.error().stage, "clock") << extra;
  }
}

TEST(ControllerTest,
     AStopByteToAWorkerAlreadyGoneIsNotAnErrorAndACleanExitWithoutAFinishIsNotClean) {
  // No ends held: the workers' pipes close as soon as the controller closes
  // its copies, so the channels end at once; the reaps come only after the
  // deadline, by which time the stop byte has nobody to go to.
  Rig rig;
  // However poll says the far side is gone -- an error condition on one
  // channel, an invalid descriptor on the other -- the channel is drained
  // and its end of file taken.
  rig.syscalls.script_readiness(kRecordRead0, platform::Readiness{false, false, true, false});
  rig.syscalls.script_readiness(kRecordRead1, platform::Readiness{false, false, false, true});
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "deadline")) {
      rig.syscalls.script_nonblocking_reaps(
          {{Reaped{kFirstPid, 0}, 0}, {Reaped{kSecondPid, 0}, 0}});
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  for (const WorkerReport& worker : result.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kTruncatedEvidence);
    EXPECT_EQ(worker.stop, "stop requested");
  }
  EXPECT_EQ(rig.syscalls.pipe_contents(kControlWrite0), "") << "EPIPE: nothing was delivered";
}

TEST(ControllerTest, TheDeadlineAsksNothingOfAWorkerAlreadyGoneOrAlreadyBeingStopped) {
  // Worker 0 finishes at once; worker 1 never beats and is terminated at tick
  // 4, before the 60 ms deadline at tick 6, which therefore asks nothing of
  // either -- and worker 1's SIGKILL falls due on the same tick.
  Rig rig;
  rig.config.run.duration = core::Duration{60};
  rig.options.stale_after_ns = 30 * kMs;
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (starts_with(line, "spawned")) {
      rig.deliver(0, ipc::RecordType::kFinished, "done early");
      rig.worker_exits(0, 0);
    } else if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
  EXPECT_EQ(result.value().workers[0].stop, "running") << "it was gone before anyone asked";
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(
      rig.reporter.events,
      (std::vector<std::string>{
          "spawned 2 worker(s): pids 1001 1002", "worker 0 (pid 1001): exited with code 0",
          "worker 1: sent signal 15 (heartbeat stale)", "deadline reached: stop requested",
          "worker 1: sent signal 9 (heartbeat stale)", "worker 1 (pid 1002): killed by signal 9"}));
}

TEST(ControllerTest, AWorkerThatGoesStaleAfterTheStopRequestIsTerminatedForStaleness) {
  // Asked to stop at tick 3; never beat, so stale at tick 5 -- before the
  // request's 100 ms grace would have sent SIGTERM for the deadline. The
  // SIGTERM is for staleness, and the fate is a timeout, not a stop.
  Rig rig;
  rig.config.run.duration = core::Duration{30};
  rig.options.stale_after_ns = 40 * kMs;
  rig.options.grace = Grace{100 * kMs, 20 * kMs};
  rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
  rig.reporter.on_event = [&rig](const std::string& line) {
    if (line == "worker 1: sent signal 9 (heartbeat stale)") {
      rig.worker_exits(0, SIGKILL);
      rig.worker_exits(1, SIGKILL);
    }
  };
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kTimedOut);
  EXPECT_EQ(rig.reporter.events[1], "deadline reached: stop requested");
  EXPECT_EQ(rig.reporter.elapsed[1], 30 * kMs);
  EXPECT_EQ(rig.reporter.events[2], "worker 0: sent signal 15 (heartbeat stale)");
  EXPECT_EQ(rig.reporter.elapsed[2], 40 * kMs);
  EXPECT_EQ(rig.reporter.elapsed[4], 60 * kMs) << "SIGKILL 20 ms after SIGTERM";
}

// --- the controller's own failures ---------------------------------------------------

TEST(ControllerTest, RefusesBeforeSpawningWhenTheConfigurationCannotBeRun) {
  {
    Rig rig;
    rig.config.workloads[0].memory = core::ByteSize::bytes(std::uint64_t{2} << 30U);
    rig.options.budget = config::MemoryBudget{std::uint64_t{1} << 30U, std::nullopt};
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "memory");
    EXPECT_EQ(rig.syscalls.fork_count(), 0);
  }
  {
    Rig rig;
    rig.config.workloads[0].enabled = false;
    rig.config.workloads[1].enabled = false;
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (ControllerError{"config", "no workload is enabled"}));
    EXPECT_EQ(describe(result.error()), "config: no workload is enabled");
  }
}

TEST(ControllerTest, EveryFailureBeforeTheLoopNamesItsStageAndLeavesNothingBehind) {
  {
    Rig rig;
    rig.syscalls.fail_ignore_signal(EINVAL);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "signals");
    EXPECT_TRUE(rig.reporter.plans.empty()) << "refused before the run was announced";
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
  {
    Rig rig;
    rig.syscalls.fail_create_memory_file(ENOMEM);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "heartbeat");
  }
  {
    Rig rig;
    rig.syscalls.fail_clock(CLOCK_MONOTONIC_RAW, EINVAL);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "clock");
  }
  {
    Rig rig;
    rig.syscalls.fail_create_pipe_on(1, EMFILE);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "record channel");
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
  {
    Rig rig;
    rig.syscalls.fail_create_pipe_on(2, EMFILE);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "control pipe");
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
  {
    Rig rig;
    rig.syscalls.script_forks({{kFirstPid, 0}, {0, EAGAIN}});
    rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "spawn");
    EXPECT_EQ(rig.syscalls.signals_sent(),
              (std::vector<std::pair<pid_t, int>>{{kFirstPid, SIGKILL}}))
        << "the pool unwound the worker it had started";
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
  for (const int fd : {kRecordWrite0, kControlRead0}) {
    Rig rig;
    rig.syscalls.fail_close_of(fd, EIO);
    // Only one reap is scripted: the teardown's wait for the second meets
    // ECHILD and stops rather than spinning on a kernel that will not say.
    rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value()) << fd;
    EXPECT_EQ(result.error().stage, "spawn");
    // Worker 0's group, then both workers. Worker 1's pid was never recorded
    // (the spawn failed on worker 0's close), so its "group" is not swept:
    // kill(-0) would be this process's own group.
    EXPECT_EQ(rig.syscalls.signals_sent(),
              (std::vector<std::pair<pid_t, int>>{
                  {-kFirstPid, SIGKILL}, {kFirstPid, SIGKILL}, {kSecondPid, SIGKILL}}))
        << "both workers were running and were killed on the way out";
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
}

TEST(ControllerTest, APipeEndNumberedZeroIsClosedLikeAnyOther) {
  // A controller started with its standard input closed is given descriptor
  // 0 for the first pipe end it creates. That is an open descriptor like any
  // other, and the teardown of a run that failed before any worker existed
  // closes it with the rest: -1 is the only number that means "not open".
  Rig rig;
  rig.syscalls.number_pipes_from(0);
  rig.syscalls.script_forks({{0, EAGAIN}});
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "spawn");
  EXPECT_EQ(rig.syscalls.close_count(), 9) << "the region and both workers' two pipes";
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerTest, EveryFailureInsideTheLoopNamesItsStageAndKillsTheWorkers) {
  const auto expect_stage = [](Rig& rig, std::string_view stage) {
    rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}, {Reaped{kSecondPid, SIGKILL}, 0}});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value()) << stage;
    EXPECT_EQ(result.error().stage, stage);
    EXPECT_EQ(rig.syscalls.signals_sent().back(), (std::pair<pid_t, int>{kSecondPid, SIGKILL}));
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  };
  {
    Rig rig;
    rig.syscalls.fail_poll(EINVAL);
    expect_stage(rig, "record channel");
  }
  {
    Rig rig;
    rig.syscalls.fail_nonblocking_reap(EIO);
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    expect_stage(rig, "reap");
  }
  {
    Rig rig;
    rig.options.stale_after_ns = 10 * kMs;
    rig.syscalls.fail_signal(EPERM);
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}, {Reaped{kSecondPid, SIGKILL}, 0}});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "signal");
  }
  {
    Rig rig;
    rig.syscalls.script_writes({{0, EIO}});
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    expect_stage(rig, "control pipe");
  }
  {
    Rig rig;
    rig.syscalls.script_readiness(kRecordRead0, platform::Readiness{true, false, false, false});
    rig.syscalls.always_fail_read(EIO);
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    expect_stage(rig, "record channel");
  }
  {
    Rig rig;
    for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
      rig.syscalls.set_clock(clock_id, platform::TimeSpec{100, 0});
    }
    rig.syscalls.advance_clocks_per_read(-10 * kMs);
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    expect_stage(rig, "clock");
  }
  {
    Rig rig;
    rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC_RAW, 4, EIO);  // The first tick's.
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    expect_stage(rig, "clock");
  }
}

TEST(ControllerTest, FailuresAtTheEndAreReportedAfterTheWorkersAreGone) {
  const auto complete_at_deadline = [](Rig& rig) {
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.reporter.on_event = [&rig](const std::string& line) {
      if (starts_with(line, "deadline")) {
        rig.deliver(0, ipc::RecordType::kFinished, "done");
        rig.deliver(1, ipc::RecordType::kFinished, "done");
        rig.worker_exits(0, 0);
        rig.worker_exits(1, 0);
      }
    };
  };
  {
    Rig rig;
    complete_at_deadline(rig);
    rig.syscalls.fail_unmap(EINVAL);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "heartbeat");
    EXPECT_TRUE(rig.reporter.reports.empty()) << "no report was told for a run that failed";
  }
  {
    Rig rig;
    complete_at_deadline(rig);
    // The finish's reading: the thirteenth, after eight in the run (the first beat, one per launch,
    // five ticks) and four in the final drain (two for each record channel still open).
    rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC_RAW, 13, EIO);
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "clock");
  }
  {
    Rig rig;
    complete_at_deadline(rig);
    // The finish's reading is earlier than the start's: a clock that jumped.
    rig.syscalls.set_clock(CLOCK_MONOTONIC_RAW, platform::TimeSpec{10, 0});
    rig.syscalls.jump_clock_on_read(CLOCK_MONOTONIC_RAW, 13, platform::TimeSpec{0, 0});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "clock");
    EXPECT_NE(result.error().detail.find("backwards"), std::string::npos) << result.error().detail;
  }
  {
    // After every worker is reaped, the final drain's poll on a channel still
    // held open fails: the controller's own failure, reported as such.
    Rig rig;
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.reporter.on_event = [&rig](const std::string& line) {
      if (starts_with(line, "spawned")) {
        rig.deliver(0, ipc::RecordType::kFinished, "done");
        rig.worker_exits(0, 0);
        rig.syscalls.worker_releases_end(kControlRead1);
        rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
        rig.syscalls.script_nonblocking_reaps(rig.reaps);
      }
    };
    rig.syscalls.fail_poll_on(2, EINVAL);  // The loop polled once; this is the final drain's.
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "record channel");
    EXPECT_NE(result.error().detail.find("poll("), std::string::npos) << result.error().detail;
  }
  {
    // The final drain is told the held channel is readable, and the read fails.
    Rig rig;
    rig.reporter.on_starting = [&rig] { rig.hold_worker_ends(); };
    rig.reporter.on_event = [&rig](const std::string& line) {
      if (starts_with(line, "spawned")) {
        rig.deliver(0, ipc::RecordType::kFinished, "done");
        rig.worker_exits(0, 0);
        rig.syscalls.worker_releases_end(kControlRead1);
        rig.reaps.push_back(FakeSyscalls::WaitStep{Reaped{kSecondPid, 0}, 0});
        rig.syscalls.script_nonblocking_reaps(rig.reaps);
      } else if (starts_with(line, "worker 1 (pid 1002)")) {
        rig.syscalls.script_readiness(kRecordRead1, platform::Readiness{true, false, false, false});
        rig.syscalls.always_fail_read(EIO);
      }
    };
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "record channel");
    EXPECT_NE(result.error().detail.find("read("), std::string::npos) << result.error().detail;
  }
  {
    Rig rig;
    complete_at_deadline(rig);
    rig.syscalls.fail_poll(EINVAL);
    rig.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}, {Reaped{kSecondPid, SIGKILL}, 0}});
    auto result = rig.run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "record channel");
  }
}

TEST(ControllerTest, ReportValuesCompareFieldByField) {
  const WorkerReport a{0, 1, WorkerFate::kCompleted, "t", "s", 2, {"r"}};
  WorkerReport b = a;
  EXPECT_EQ(a, b);
  b.slot = 9;
  EXPECT_NE(a, b);
  b = a;
  b.pid = 9;
  EXPECT_NE(a, b);
  b = a;
  b.fate = WorkerFate::kFault;
  EXPECT_NE(a, b);
  b = a;
  b.termination = "x";
  EXPECT_NE(a, b);
  b = a;
  b.stop = "x";
  EXPECT_NE(a, b);
  b = a;
  b.progress = 9;
  EXPECT_NE(a, b);
  b = a;
  b.records = {};
  EXPECT_NE(a, b);

  RunReport r;
  r.workers = {a};
  r.elapsed_ns = 5;
  RunReport s = r;
  EXPECT_EQ(r, s);
  s.elapsed_ns = 6;
  EXPECT_NE(r, s);
  s = r;
  s.worker_exit_code = 120;
  EXPECT_NE(r, s);
  s = r;
  s.workers.clear();
  EXPECT_NE(r, s);
  EXPECT_FALSE(all_clean(s)) << "no workers is not all clean";

  const RunOptions o;
  RunOptions p = o;
  EXPECT_EQ(o, p);
  p.executable = "x";
  EXPECT_NE(o, p);
  p = o;
  p.interval_ns = 1;
  EXPECT_NE(o, p);
  p = o;
  p.stale_after_ns = 1;
  EXPECT_NE(o, p);
  p = o;
  p.grace.kill_ns = 1;
  EXPECT_NE(o, p);
  p = o;
  config::MemoryBudget budget;
  budget.available_bytes = 1;
  p.budget = budget;
  EXPECT_NE(o, p);
  p = o;
  p.budget_unread = "x";
  EXPECT_NE(o, p);
  p = o;
  ScriptedOomRecord record;
  p.oom_record = &record;
  EXPECT_NE(o, p);
  p = o;
  const OneCpuSysfs tree;
  auto snapshot = tree.snapshot();
  ASSERT_TRUE(snapshot.has_value());
  p.topology = &snapshot.value();
  EXPECT_NE(o, p);

  const RunPlan plan;
  RunPlan other = plan;
  EXPECT_EQ(plan, other);
  other.mode = config::Mode::kExplore;
  EXPECT_NE(plan, other);
  other = plan;
  other.duration = core::Duration{1};
  EXPECT_NE(plan, other);
  other = plan;
  other.workers = 1;
  EXPECT_NE(plan, other);
  other = plan;
  other.executable = "x";
  EXPECT_NE(plan, other);
  other = plan;
  other.limits.push_back(LimitStatus{});
  EXPECT_NE(plan, other);
  other = plan;
  other.topology = "x";
  EXPECT_NE(plan, other);

  const ControllerError e{"a", "b"};
  EXPECT_EQ(e, (ControllerError{"a", "b"}));
  EXPECT_NE(e, (ControllerError{"x", "b"}));
  EXPECT_NE(e, (ControllerError{"a", "x"}));
}

}  // namespace
}  // namespace loadforge::controller
