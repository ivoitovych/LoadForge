// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7: the controller against real workers. Every test runs N >= 2 workers
// (docs/PLAN.md §4.6): with one, "noticed a death" and "noticed the only child
// exited" are the same observation.

#include "controller/supervision.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "core/duration.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;

config::Config two_null_workloads(core::Duration duration) {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = duration;
  config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
  config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
  return config;
}

RunOptions quick_options() {
  RunOptions options;
  options.executable = LOADFORGE_BINARY;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = 2000 * kMs;
  options.grace = Grace{500 * kMs, 500 * kMs};
  return options;
}

/// The pids the controller announced, parsed from its "spawned" line.
std::vector<pid_t> pids_in(std::string_view line) {
  std::vector<pid_t> pids;
  const std::size_t colon = line.find("pids");
  if (colon == std::string_view::npos) {
    return pids;
  }
  std::istringstream rest{std::string{line.substr(colon + 4)}};
  pid_t pid = 0;
  while (rest >> pid) {
    pids.push_back(pid);
  }
  return pids;
}

/// A reporter that keeps everything and can act on the "spawned" event --
/// the one moment a test can reach into a run that otherwise owns the
/// thread.
class WatchingReporter final : public Reporter {
 public:
  void run_starting(const RunPlan& plan) override { plans.push_back(plan); }
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    events.emplace_back(line);
    if (line.substr(0, 7) == "spawned" && on_spawned) {
      on_spawned(pids_in(line));
    }
  }
  void run_finished(const RunReport& report) override { reports.push_back(report); }

  std::vector<RunPlan> plans;
  std::vector<std::string> events;
  std::vector<RunReport> reports;
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

TEST(SupervisionTest, TwoWorkersCompleteForTheConfiguredDuration) {
  platform::RealSyscalls syscalls;
  WatchingReporter reporter;
  Controller controller(syscalls, reporter);
  auto report = controller.run(two_null_workloads(core::Duration{600}), quick_options());
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  ASSERT_FALSE(report.value().worker_exit_code.has_value());
  ASSERT_EQ(report.value().workers.size(), 2U);
  for (const WorkerReport& worker : report.value().workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kCompleted)
        << describe(worker.fate) << ": " << worker.termination;
    EXPECT_EQ(worker.termination, "exited with code 0");
    EXPECT_GT(worker.progress, 5U) << "it beat every 20 ms for 600 ms";
    EXPECT_TRUE(has_record(worker, "started: slot " + std::to_string(worker.slot)));
    EXPECT_TRUE(has_record(worker, "finished: "));
    EXPECT_GT(worker.pid, 0);
  }
  EXPECT_NE(report.value().workers[0].pid, report.value().workers[1].pid);
  EXPECT_GE(report.value().elapsed_ns, 600 * kMs);
  EXPECT_TRUE(all_clean(report.value()));
  ASSERT_EQ(reporter.plans.size(), 1U);
  EXPECT_EQ(reporter.plans[0].workers, 2U);
  ASSERT_EQ(reporter.reports.size(), 1U);
  EXPECT_EQ(reporter.reports[0], report.value());
  ASSERT_FALSE(reporter.events.empty());
  EXPECT_EQ(reporter.events[0].substr(0, 22), "spawned 2 worker(s): p");
}

TEST(SupervisionTest, TheMachineIsDescribedOnceBeforeTheFirstWorkerIsForked) {
  // START -> RUNNING in the state machine of IMPLEMENTATION §4 M1: the
  // topology snapshot is taken before the fork. Taken here from this
  // machine's /sys as `loadforge stress` takes it, handed to a run of two real
  // workers, and found in the plan, which reaches the reporter before the
  // "spawned" event and so before any worker existed. The count is checked
  // against sysconf, a source discovery does not read.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto machine = topology::take_snapshot(filesystem);
  ASSERT_TRUE(machine.has_value()) << topology::describe(machine.error());
  const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
  ASSERT_GT(online, 0);
  EXPECT_EQ(machine.value().cpus.logical_cpu_count(), static_cast<std::size_t>(online));

  WatchingReporter reporter;
  bool planned_before_spawn = false;
  reporter.on_spawned = [&reporter, &planned_before_spawn](const std::vector<pid_t>& pids) {
    EXPECT_EQ(pids.size(), 2U);
    planned_before_spawn = reporter.plans.size() == 1;
  };
  RunOptions options = quick_options();
  options.topology = &machine.value();
  Controller controller(syscalls, reporter);
  auto report = controller.run(two_null_workloads(core::Duration{300}), options);
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  EXPECT_TRUE(all_clean(report.value()));
  EXPECT_TRUE(planned_before_spawn) << "the plan came after the workers";
  ASSERT_EQ(reporter.plans.size(), 1U);
  EXPECT_EQ(reporter.plans[0].topology, topology::describe(machine.value()));
  EXPECT_EQ(reporter.plans[0].topology.substr(0, std::to_string(online).size() + 15),
            std::to_string(online) + " logical CPU(s)");
}

TEST(SupervisionTest, AWorkerKilledFromOutsideIsReportedKilledWhileTheOtherCompletes) {
  platform::RealSyscalls syscalls;
  WatchingReporter reporter;
  reporter.on_spawned = [](const std::vector<pid_t>& pids) {
    ASSERT_EQ(pids.size(), 2U);
    ASSERT_EQ(::kill(pids[1], SIGKILL), 0);
  };
  Controller controller(syscalls, reporter);
  auto report = controller.run(two_null_workloads(core::Duration{600}), quick_options());
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  ASSERT_EQ(report.value().workers.size(), 2U);
  EXPECT_EQ(report.value().workers[0].fate, WorkerFate::kCompleted);
  EXPECT_EQ(report.value().workers[1].fate, WorkerFate::kKilled);
  EXPECT_EQ(report.value().workers[1].termination, "killed by signal 9");
  EXPECT_FALSE(all_clean(report.value()));
  EXPECT_EQ(clean_count(report.value()), 1U);
}

TEST(SupervisionTest, AControllerKilledLeavesNoWorkerBehind) {
  // The dangerous transition: the controller dies and the workers must not
  // outlive it. The controller runs in a forked child with a long duration;
  // it tells this process the worker pids through a pipe; this process kills
  // it and then, as the subreaper the orphans fall to, reaps each worker and
  // reads how it died.
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
  int fds[2] = {-1, -1};
  // Close-on-exec, or the workers inherit the write end through the
  // controller's fork and the end of file this process waits for arrives
  // only when the last worker exits -- which the first version proved by
  // waiting the whole sixty seconds.
  ASSERT_EQ(::pipe2(fds, O_CLOEXEC), 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::close(fds[0]);
    platform::RealSyscalls syscalls;
    WatchingReporter reporter;
    reporter.on_spawned = [write_fd = fds[1]](const std::vector<pid_t>& pids) {
      std::string line;
      for (const pid_t pid : pids) {
        line += std::to_string(pid) + "\n";
      }
      (void)::write(write_fd, line.data(), line.size());
      ::close(write_fd);
    };
    Controller controller(syscalls, reporter);
    (void)controller.run(two_null_workloads(core::Duration{60'000}), quick_options());
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
  for (const pid_t worker : workers) {
    EXPECT_EQ(::kill(worker, 0), 0) << "worker " << worker << " alive before the controller dies";
  }

  ASSERT_EQ(::kill(child, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  // Each orphan is reparented here and must die within a bounded time by the
  // death signal the launcher armed.
  for (const pid_t worker : workers) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    pid_t reaped = 0;
    int worker_status = 0;
    while (reaped == 0 && std::chrono::steady_clock::now() < deadline) {
      reaped = ::waitpid(worker, &worker_status, WNOHANG);
      if (reaped == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    ASSERT_EQ(reaped, worker) << "worker " << worker
                              << " outlived the controller: " << std::strerror(errno);
    // Two ways out, and either is the worker not outliving its controller. The
    // kernel delivers the parent-death signal; and the controller's death
    // closes its end of the control pipe, which wakes the worker's poll. Which
    // lands first is the scheduler's choice: an earlier version of this test
    // accepted only SIGKILL and failed in about one run in ten when the worker
    // left first, exiting 124, controller lost. What this test cannot show is
    // that the death signal works for a worker that never looks at its pipe:
    // with the signal disarmed it still passes, every worker leaving on the
    // hang-up. MisbehaviourCatalogue.AWorkerDeafToItsControlPipeIsEndedByThe-
    // ParentDeathSignal (catalogue_test.cpp) is that test: its worker ignores
    // the pipe and can only be ended by the signal.
    const bool killed_by_death_signal =
        WIFSIGNALED(worker_status) && WTERMSIG(worker_status) == SIGKILL;
    const bool left_on_hang_up = WIFEXITED(worker_status) && WEXITSTATUS(worker_status) == 124;
    EXPECT_TRUE(killed_by_death_signal || left_on_hang_up) << "status " << worker_status;
  }
  ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 0), 0);
}

}  // namespace
}  // namespace loadforge::controller
