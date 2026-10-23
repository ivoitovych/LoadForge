// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIGINT and SIGTERM in the controller (journal 34b): the first stops every
// worker as the deadline does, a later one kills them at once, and the report,
// the events and the journal say which. Over the fake seam, where a signal is
// a byte the test puts in the pipe the handler would have written to, from
// inside the reporter's callbacks, the only place a test can act while the
// loop owns the thread.

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "controller/stop_signals.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/journal_model.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::Reaped;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;
constexpr pid_t kFirstPid = 1001;
constexpr pid_t kSecondPid = 1002;
// The signals' pipe is made first (200, 201); then per worker a record pipe and
// a control pipe.
constexpr int kStopRead = 200;
constexpr int kRecordWrite0 = 203;
constexpr int kControlRead0 = 204;
constexpr int kControlWrite0 = 205;
constexpr int kRecordWrite1 = 207;
constexpr int kControlRead1 = 208;

bool starts_with(std::string_view line, std::string_view prefix) {
  return line.substr(0, prefix.size()) == prefix;
}

class Plant final : public Reporter {
 public:
  void run_starting(const RunPlan& plan) override {
    plans.push_back(plan);
    for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
      syscalls->worker_holds_end(fd);
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
  void isolation_finished(const RunReport& /*report*/) override {}

  void leave_first() const {
    syscalls->worker_releases_end(kRecordWrite0);
    syscalls->worker_releases_end(kControlRead0);
    syscalls->child_exits(kFirstPid);
    syscalls->script_nonblocking_reaps({{Reaped{kFirstPid, 0}, 0}});
  }
  void leave_second() const {
    syscalls->worker_releases_end(kRecordWrite1);
    syscalls->worker_releases_end(kControlRead1);
    syscalls->child_exits(kSecondPid);
    syscalls->script_nonblocking_reaps({{Reaped{kSecondPid, 0}, 0}});
  }
  void leave() const {
    for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
      syscalls->worker_releases_end(fd);
    }
    syscalls->child_exits(kFirstPid);
    syscalls->child_exits(kSecondPid);
    syscalls->script_nonblocking_reaps({{Reaped{kFirstPid, 0}, 0}, {Reaped{kSecondPid, 0}, 0}});
  }

  [[nodiscard]] std::size_t count(std::string_view part) const {
    return static_cast<std::size_t>(std::ranges::count_if(events, [part](const std::string& event) {
      return event.find(part) != std::string::npos;
    }));
  }
  [[nodiscard]] bool told(std::string_view part) const { return count(part) > 0; }

  FakeSyscalls* syscalls = nullptr;
  std::function<void(const std::string&)> on_event;
  std::vector<RunPlan> plans;
  std::vector<std::string> events;
  std::vector<std::int64_t> elapsed;
  std::vector<RunReport> reports;
};

/// A run of two workers for five seconds, with the signals installed.
struct Interrupted {
  FakeSyscalls syscalls;
  Plant reporter;
  StopSignals stop{syscalls};
  config::Config config;
  RunOptions options;

  Interrupted() {
    EXPECT_TRUE(stop.install().has_value());
    reporter.syscalls = &syscalls;
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{5000};
    config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
    config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
    options.executable = "/opt/loadforge";
    options.interval_ns = 10 * kMs;
    options.stale_after_ns = 100'000 * kMs;
    options.grace = Grace{20 * kMs, 20 * kMs};
    options.stop_signals = &stop;
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.script_forks({{kFirstPid, 0}, {kSecondPid, 0}});
  }

  /// The signal arrives once the workers are spawned; they leave when asked to stop.
  void signal_after_spawn(int signal) {
    reporter.on_event = [this, signal](const std::string& line) {
      if (starts_with(line, "spawned")) {
        EXPECT_TRUE(syscalls.raise_diverted(signal));
      }
      if (line.find(" received: stop requested") != std::string::npos) {
        reporter.leave();
      }
    };
  }

  core::Result<RunReport, ControllerError> go() {
    Controller controller(syscalls, reporter);
    return controller.run(config, options);
  }
};

TEST(OrchestrationStopSignalTest, SigintStopsEveryWorkerAsTheDeadlineDoesAndTheReportSaysSo) {
  Interrupted run;
  run.signal_after_spawn(SIGINT);

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const RunReport& report = result.value();
  ASSERT_TRUE(report.stop_signal.has_value());
  EXPECT_EQ(*report.stop_signal, SIGINT);
  EXPECT_FALSE(report.stop_forced);
  EXPECT_EQ(describe_stop_signal(report), "stopped by SIGINT");
  EXPECT_TRUE(run.reporter.told("SIGINT received: stop requested"));
  EXPECT_FALSE(run.reporter.told("deadline reached")) << "long before it";
  EXPECT_FALSE(run.reporter.told("another signal"));
  ASSERT_EQ(report.workers.size(), 2U);
  for (const WorkerReport& worker : report.workers) {
    EXPECT_EQ(worker.commands.sent, 1U) << "a Stop, as at the deadline";
    EXPECT_EQ(worker.stop, "stop requested");
  }
  EXPECT_LT(report.elapsed_ns, 5000 * kMs);
  EXPECT_FALSE(run.reporter.told("sent signal")) << "the grace was kept";
  run.stop.release();
  EXPECT_TRUE(run.syscalls.all_descriptors_closed());
}

TEST(OrchestrationStopSignalTest, SigtermStopsTheRunToo) {
  Interrupted run;
  run.signal_after_spawn(SIGTERM);

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().stop_signal, std::optional<int>{SIGTERM});
  EXPECT_TRUE(run.reporter.told("SIGTERM received: stop requested"));
  EXPECT_EQ(describe_stop_signal(result.value()), "stopped by SIGTERM");
}

TEST(OrchestrationStopSignalTest, ASignalWhileTheWorkersDoNotLeaveIsEscalatedByTheGraceAsEver) {
  Interrupted run;
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "spawned")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    // They leave only when killed: the SIGTERM after the first grace went unheeded.
    if (starts_with(line, "worker 1: sent signal 9")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_TRUE(run.reporter.told("worker 0: sent signal 15"));
  EXPECT_TRUE(run.reporter.told("worker 0: sent signal 9"));
  EXPECT_FALSE(result.value().stop_forced) << "the grace, not the user, ended them";
}

TEST(OrchestrationStopSignalTest, ASecondSignalKillsTheWorkersAtOnceWithoutTheirGrace) {
  Interrupted run;
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "spawned")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    if (starts_with(line, "SIGINT received")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGTERM));
    }
    if (starts_with(line, "worker 1: sent signal 9")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const RunReport& report = result.value();
  EXPECT_EQ(report.stop_signal, std::optional<int>{SIGINT}) << "the first one is the cause";
  EXPECT_TRUE(report.stop_forced);
  EXPECT_EQ(describe_stop_signal(report),
            "stopped by SIGINT; a later signal killed the workers at once");
  EXPECT_TRUE(run.reporter.told("another signal received: killing the workers"));
  EXPECT_FALSE(run.reporter.told("sent signal 15")) << "no SIGTERM, no grace";
  const auto& sent = run.syscalls.signals_sent();
  EXPECT_NE(std::ranges::find(sent, std::pair<pid_t, int>{kFirstPid, SIGKILL}), sent.end());
  EXPECT_NE(std::ranges::find(sent, std::pair<pid_t, int>{kSecondPid, SIGKILL}), sent.end());
  for (const WorkerReport& worker : report.workers) {
    EXPECT_EQ(worker.stop, "SIGKILL sent");
  }
}

TEST(OrchestrationStopSignalTest, TwoSignalsInOneLookAreTheStopAndTheKill) {
  Interrupted run;
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "spawned")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    if (starts_with(line, "worker 1: sent signal 9")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_TRUE(result.value().stop_forced);
  EXPECT_EQ(run.reporter.count("SIGINT received: stop requested"), 1U);
  EXPECT_EQ(run.reporter.count("another signal received"), 1U);
}

TEST(OrchestrationStopSignalTest, ALaterSignalSkipsAWorkerAlreadyKilledAndOneAlreadyGone) {
  Interrupted run;
  int insisted = 0;
  run.reporter.on_event = [&run, &insisted](const std::string& line) {
    if (starts_with(line, "spawned")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    if (starts_with(line, "SIGINT received")) {
      run.reporter.leave_first();  // The first worker is gone before the user insists.
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    if (starts_with(line, "another signal received")) {
      if (++insisted == 1) {
        EXPECT_TRUE(run.syscalls.raise_diverted(SIGTERM));  // And again, after the kill.
      } else {
        run.reporter.leave_second();  // Gone before the third signal's look, killed already.
      }
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(run.reporter.count("another signal received"), 2U);
  const auto& sent = run.syscalls.signals_sent();
  EXPECT_EQ(std::ranges::count(sent, std::pair<pid_t, int>{kSecondPid, SIGKILL}), 1)
      << "killed once, whatever the number of signals";
  EXPECT_EQ(std::ranges::count(sent, std::pair<pid_t, int>{kFirstPid, SIGKILL}), 0)
      << "the one already gone is not signalled";
}

TEST(OrchestrationStopSignalTest, ASignalAfterTheDeadlineHasAskedDoesNotAskTheWorkersAgain) {
  Interrupted run;
  run.config.run.duration = core::Duration{50};
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "deadline reached")) {
      EXPECT_TRUE(run.syscalls.raise_diverted(SIGINT));
    }
    if (starts_with(line, "SIGINT received")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().stop_signal, std::optional<int>{SIGINT});
  EXPECT_EQ(result.value().workers[0].commands.sent, 1U) << "asked once, by the deadline";
  EXPECT_EQ(result.value().workers[1].commands.sent, 1U);
  EXPECT_TRUE(run.reporter.told("deadline reached: stop requested"));
}

TEST(OrchestrationStopSignalTest, TheJournalSaysWhichSignalStoppedTheRunAndEndsWithDone) {
  Interrupted run;
  run.signal_after_spawn(SIGTERM);
  controller_testing::RecordingWriter journal;
  run.options.journal = &journal;

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const auto entry = std::ranges::find_if(journal.entries, [](const journal::Entry& each) {
    return starts_with(each.phase, "stopped by");
  });
  ASSERT_NE(entry, journal.entries.end());
  EXPECT_EQ(entry->phase, "stopped by SIGTERM");
  EXPECT_EQ(entry->workers_alive, 2U);
  EXPECT_EQ(journal.entries.back().phase, "done");
}

TEST(OrchestrationStopSignalTest, ARunWithoutSignalsCaughtIsNotStoppedByOne) {
  Interrupted run;
  run.options.stop_signals = nullptr;
  run.config.run.duration = core::Duration{50};
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "deadline reached")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().stop_signal.has_value());
  EXPECT_FALSE(result.value().stop_forced);
  EXPECT_EQ(describe_stop_signal(result.value()), "");
}

TEST(OrchestrationStopSignalTest, ARunNobodyInterruptedHasNoSignalAndWaitsOnThePipeToo) {
  Interrupted run;
  run.config.run.duration = core::Duration{50};
  run.reporter.on_event = [&run](const std::string& line) {
    if (starts_with(line, "deadline reached")) {
      run.reporter.leave();
    }
  };

  auto result = run.go();

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().stop_signal.has_value());
  EXPECT_TRUE(run.stop.received().empty());
  EXPECT_GT(run.syscalls.poll_count(), 0);
}

// ---- the signals' own pipe failing --------------------------------------------------

class StopPipePollFails : public ::testing::TestWithParam<int> {};

TEST_P(StopPipePollFails, TheRunEndsWithTheKernelsErrorAndNoWorkerIsLeftBehind) {
  Interrupted run;
  run.signal_after_spawn(SIGINT);
  run.syscalls.fail_poll_on(2, GetParam());  // The look at the pipe, after the tick's wait.

  auto result = run.go();

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "stop signal");
  EXPECT_NE(result.error().detail.find("poll"), std::string::npos) << result.error().detail;
  EXPECT_TRUE(run.reporter.reports.empty());
  run.stop.release();
  EXPECT_TRUE(run.syscalls.all_descriptors_closed());
}

// poll(2): EFAULT, EINTR (RealSyscalls answers it itself; here the seam says it), EINVAL, ENOMEM.
INSTANTIATE_TEST_SUITE_P(PollErrors, StopPipePollFails,
                         ::testing::Values(EFAULT, EINTR, EINVAL, ENOMEM));

class StopPipeReadFails : public ::testing::TestWithParam<int> {};

TEST_P(StopPipeReadFails, TheRunEndsWithTheKernelsErrorAndNoWorkerIsLeftBehind) {
  Interrupted run;
  run.signal_after_spawn(SIGINT);
  run.syscalls.script_reads({FakeSyscalls::ReadStep{"", GetParam()}});

  auto result = run.go();

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "stop signal");
  EXPECT_NE(result.error().detail.find("read"), std::string::npos) << result.error().detail;
  run.stop.release();
  EXPECT_TRUE(run.syscalls.all_descriptors_closed());
}

// read(2): EAGAIN, EBADF, EFAULT, EINTR, EINVAL, EIO, EISDIR.
INSTANTIATE_TEST_SUITE_P(ReadErrors, StopPipeReadFails,
                         ::testing::Values(EAGAIN, EBADF, EFAULT, EINTR, EINVAL, EIO, EISDIR));

// ---- values ---------------------------------------------------------------------------

TEST(OrchestrationStopSignalTest, TheSignalsTakePartInTheComparisons) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  const RunOptions o;
  RunOptions p = o;
  EXPECT_EQ(o, p);
  p.stop_signals = &stop;
  EXPECT_NE(o, p);

  const RunReport r;
  RunReport s = r;
  s.stop_signal = SIGINT;
  EXPECT_NE(r, s);
  s = r;
  s.stop_forced = true;
  EXPECT_NE(r, s);
}

}  // namespace
}  // namespace loadforge::controller
