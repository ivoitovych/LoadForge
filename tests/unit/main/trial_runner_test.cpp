// SPDX-License-Identifier: GPL-3.0-or-later
//
// The trial runner over the controller (main/trial_runner.hpp), over the fake of
// the system calls: what makes a candidate unrunnable, what makes a trial a
// controlled experiment (the settle phase, the load window), and every way the
// run underneath it can fail. Real processes are tests/integration/m6.

#include "main/trial_runner.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "contracts/explore/trial_contract.hpp"
#include "main/telemetry_rig.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/machine_tree.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

using explore::Candidate;
using explore::Settle;
using explore::TrialFault;

constexpr std::int64_t kMs = 1'000'000;
constexpr int kClockMonotonic = 1;

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// --- sources a test scripts ---------------------------------------------------------------

/// A gauge that rises by `step` on every reading, or reads as gone.
class LevelSource final : public telemetry::Source {
 public:
  LevelSource(double step, bool blind) : step_(step), blind_(blind) {}
  [[nodiscard]] std::string_view name() const noexcept override { return "level"; }
  std::vector<telemetry::MetricStatus> probe() override {
    return {{telemetry::Metric{"level.reading", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                               telemetry::Scope::kPackage, 0, std::nullopt, "test"},
             std::nullopt}};
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    if (blind_) {
      out.emplace_back(telemetry::Unavailable{topology::Availability::kUnreadable, "/sys/level",
                                              "the sensor stopped answering", "reseat it"});
      return;
    }
    value_ += step_;
    out.emplace_back(telemetry::Gauge{value_});
  }

 private:
  double step_;
  bool blind_;
  double value_ = 40.0;
};

std::vector<std::unique_ptr<telemetry::Source>> flat_level(platform::Syscalls& /*s*/,
                                                           platform::FileSystem& /*f*/,
                                                           const MachineRoots& /*r*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<LevelSource>(0.0, false));
  return sources;
}

std::vector<std::unique_ptr<telemetry::Source>> rising_level(platform::Syscalls& /*s*/,
                                                             platform::FileSystem& /*f*/,
                                                             const MachineRoots& /*r*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<LevelSource>(5.0, false));
  return sources;
}

std::vector<std::unique_ptr<telemetry::Source>> blind_level(platform::Syscalls& /*s*/,
                                                            platform::FileSystem& /*f*/,
                                                            const MachineRoots& /*r*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<LevelSource>(0.0, true));
  return sources;
}

/// More series than a timeline sized for a machine can take a few rounds of.
class CrowdSource final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "crowd"; }
  std::vector<telemetry::MetricStatus> probe() override {
    std::vector<telemetry::MetricStatus> statuses;
    for (std::uint32_t index = 0; index < kSeries; ++index) {
      statuses.push_back({telemetry::Metric{"crowd.m" + std::to_string(index),
                                            telemetry::Unit::kCount, telemetry::Kind::kGauge,
                                            telemetry::Scope::kMachine, 0, std::nullopt, "test"},
                          std::nullopt});
    }
    return statuses;
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    for (std::uint32_t index = 0; index < kSeries; ++index) {
      out.emplace_back(telemetry::Gauge{1.0});
    }
  }

 private:
  static constexpr std::uint32_t kSeries = 20'000;
};

std::vector<std::unique_ptr<telemetry::Source>> crowd(platform::Syscalls& /*s*/,
                                                      platform::FileSystem& /*f*/,
                                                      const MachineRoots& /*r*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<CrowdSource>());
  return sources;
}

class TwinSource final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "twin"; }
  std::vector<telemetry::MetricStatus> probe() override {
    return {{telemetry::Metric{"twin.value", telemetry::Unit::kCount, telemetry::Kind::kGauge,
                               telemetry::Scope::kMachine, 0, std::nullopt, "test"},
             std::nullopt}};
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    out.emplace_back(telemetry::Gauge{1});
  }
};

std::vector<std::unique_ptr<telemetry::Source>> twins(platform::Syscalls& /*s*/,
                                                      platform::FileSystem& /*f*/,
                                                      const MachineRoots& /*r*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<TwinSource>());
  sources.push_back(std::make_unique<TwinSource>());
  return sources;
}

/// A summarizer that fails on its Nth call (1-based), and otherwise is the real one.
class FailingSummarizer final : public statistics::Summarizer {
 public:
  explicit FailingSummarizer(int failing_call) : failing_call_(failing_call) {}
  [[nodiscard]] core::Result<statistics::LoadSignature, statistics::StatisticsError> summarize(
      const telemetry::TimelineView& view, const statistics::Window& window) const override {
    if (++calls_ == failing_call_) {
      return statistics::StatisticsError{statistics::StatisticsFault::kWindow, "scripted"};
    }
    return real_.summarize(view, window);
  }

 private:
  int failing_call_;
  mutable int calls_ = 0;
  statistics::CompensatedSummarizer real_;
};

// --- the machine a test runs on -----------------------------------------------------------

/// The fake of the system calls with memory and CPU ticks served as the kernel
/// shows them, and a clock that moves 10 ms on every read. A run that reaches its
/// deadline needs workers that live until then, which the fake cannot give the
/// controller's own reporter: that run is tests/integration/m6.
struct Machine {
  Machine() {
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "meminfo" / "present" / "proc",
                      "/proc");
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "stat" / "present" / "proc",
                      "/proc");
    options.executable = "/opt/loadforge";
    options.sample_interval_ns = 30 * kMs;
  }

  /// `count` workers started; they exit at once, so the run ends before its
  /// deadline.
  void workers_that_exit_at_once(int count) {
    std::vector<testing::FakeSyscalls::ForkStep> forks;
    std::vector<testing::FakeSyscalls::WaitStep> reaps;
    for (int worker = 0; worker < count; ++worker) {
      forks.push_back({1001 + worker, 0});
      reaps.push_back({platform::Reaped{1001 + worker, 0}, 0});
    }
    syscalls.script_forks(forks);
    syscalls.script_nonblocking_reaps(reaps);
  }

  std::unique_ptr<ControllerTrialRunner> runner() {
    auto made = ControllerTrialRunner::create(syscalls, snapshot, registry, options);
    EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error().detail);
    return made.has_value() ? std::move(made).value() : nullptr;
  }

  /// One CPU with a cache of 64-byte lines: the workloads refuse a machine whose
  /// line size is not known.
  controller_testing::MachineTree tree{controller_testing::MachineShape{1, 64, true}};
  topology::Snapshot snapshot = tree.accepted();
  testing::FakeSyscalls syscalls;
  workloads::BuiltinRegistry registry;
  TrialOptions options;
};

Candidate a_candidate() { return Candidate{"probe", {"sort"}, 1, {}, 40 * kMs, std::nullopt, {}}; }

// --- create -------------------------------------------------------------------------------

TEST(TrialRunnerCreate, ProbesTheTelemetryOnceAndKeepsTheReport) {
  Machine machine;
  auto runner = machine.runner();
  ASSERT_NE(runner, nullptr);
  const telemetry::CapabilityCounts counts = telemetry::count(runner->capabilities());
  EXPECT_GT(counts.present, 0U) << "memory and the CPU ticks are served";
  EXPECT_LT(counts.present, counts.metrics) << "and no thermal sensor is";
  EXPECT_EQ(runner->trials_started(), 0U);
  EXPECT_FALSE(runner->worker_exit_code().has_value());
}

TEST(TrialRunnerCreate, TelemetryTheTimelineRefusesIsAKRunBeforeAnyTrial) {
  Machine machine;
  machine.options.sources = twins;
  const auto made = ControllerTrialRunner::create(machine.syscalls, machine.snapshot,
                                                  machine.registry, machine.options);
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(made.error().detail, "cannot set up telemetry: duplicate series"))
      << made.error().detail;
  EXPECT_EQ(machine.syscalls.fork_count(), 0);
}

// --- an unrunnable candidate ----------------------------------------------------------------

TEST(TrialRunnerCandidate, EachWayACandidateCannotRunIsRefusedByName) {
  Machine machine;
  auto runner = machine.runner();
  struct Case {
    void (*spoil)(Candidate&);
    const char* says;
  };
  const std::vector<Case> cases{
      {[](Candidate& c) { c.workloads.clear(); }, "names no workload"},
      {[](Candidate& c) { c.workloads = {"no_such"}; },
       "no workload named 'no_such'; this build has: "},
      {[](Candidate& c) { c.threads = 0; }, "has no threads"},
      {[](Candidate& c) { c.threads = 1025; }, "more than 1024 worker processes"},
      {[](Candidate& c) { c.workloads = {"sort", "fft"}; }, "fewer threads than workloads"},
      {[](Candidate& c) { c.duration_ns = 0; }, "duration must be above 0"},
      {[](Candidate& c) { c.duration_ns = -5; }, "duration must be above 0"},
      {[](Candidate& c) { c.duration_ns = std::int64_t{31} * 24 * 3600 * 1'000'000'000; },
       "at most 30 days"},
      {[](Candidate& c) { c.cpus = {0, 7}; }, "CPU 7 is not one this process may use"},
      {[](Candidate& c) { c.settle = Settle{"memory.available", 0.05, 0, 10 * kMs}; },
       "settle phase needs a window"},
      {[](Candidate& c) { c.settle = Settle{"memory.available", 0.05, 10 * kMs, 0}; },
       "settle phase needs a window"},
      {[](Candidate& c) {
         c.settle = Settle{"memory.available", 0.05, std::int64_t{31} * 24 * 3600 * 1'000'000'000,
                           10 * kMs};
       },
       "settle phase needs a window"},
      {[](Candidate& c) {
         c.settle = Settle{"memory.available", 0.05, 10 * kMs,
                           std::int64_t{31} * 24 * 3600 * 1'000'000'000};
       },
       "settle phase needs a window"},
      {[](Candidate& c) { c.settle = Settle{"memory.available", -1.0, 10 * kMs, 10 * kMs}; },
       "settle phase needs a window"},
      {[](Candidate& c) {
         c.settle = Settle{"memory.available", std::nan(""), 10 * kMs, 10 * kMs};
       },
       "settle phase needs a window"},
  };
  for (const Case& each : cases) {
    Candidate candidate = a_candidate();
    each.spoil(candidate);
    const auto outcome = runner->run(candidate);
    ASSERT_FALSE(outcome.has_value()) << each.says;
    EXPECT_EQ(outcome.error().fault, TrialFault::kCandidate) << each.says;
    EXPECT_TRUE(contains(outcome.error().detail, each.says)) << outcome.error().detail;
    EXPECT_TRUE(contains(outcome.error().detail, "'probe'")) << "the candidate is named";
  }
  EXPECT_EQ(machine.syscalls.fork_count(), 0) << "nothing was started for any of them";
  EXPECT_EQ(runner->trials_started(), 0U);
}

TEST(TrialRunnerCandidate, ASettleMetricThatCannotBeReadIsRefusedWithTheSourcesOwnRemedy) {
  Machine machine;
  auto runner = machine.runner();
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"thermal.package", 0.05, 10 * kMs, 100 * kMs};
  auto outcome = runner->run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kMetricUnavailable);
  EXPECT_TRUE(contains(outcome.error().detail, "'thermal.package' is not readable on this machine"))
      << outcome.error().detail;
  EXPECT_TRUE(contains(outcome.error().detail, "hwmon"))
      << "the remedy: " << outcome.error().detail;

  candidate.settle->metric = "thermal.no_such_sensor";
  outcome = runner->run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kMetricUnavailable);
  EXPECT_TRUE(contains(outcome.error().detail, "is not a gauge in the capability report"));

  // A counter is there, and has no slope to settle on.
  const telemetry::CapabilityReport report = runner->capabilities();
  std::string counter;
  for (const auto& source : report.sources) {
    for (const auto& status : source.metrics) {
      if (status.metric.kind == telemetry::Kind::kCounter && !status.unavailable.has_value()) {
        counter = status.metric.name;
      }
    }
  }
  ASSERT_FALSE(counter.empty()) << "the CPU ticks are a counter";
  candidate.settle->metric = counter;
  outcome = runner->run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kMetricUnavailable);
  EXPECT_EQ(machine.syscalls.fork_count(), 0);
}

// --- the run underneath ------------------------------------------------------------------------

TEST(TrialRunnerRun, AnUnfinishedRunStillHasTheWindowItsDurationAsksFor) {
  Machine machine;
  machine.workers_that_exit_at_once(1);
  auto runner = machine.runner();
  const auto outcome = runner->run(a_candidate());
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  const statistics::Window window = outcome.value().signature.window;
  EXPECT_EQ(window.end_ns - window.start_ns, 40 * kMs);
  EXPECT_EQ(outcome.value().verdict, results::Verdict::kIncomplete)
      << "a worker that left at once did not finish";
  EXPECT_EQ(outcome.value().candidate, "probe");
  EXPECT_FALSE(outcome.value().settled);
  EXPECT_EQ(outcome.value().settle_ns, 0);
  EXPECT_EQ(runner->trials_started(), 1U);
  EXPECT_EQ(machine.syscalls.fork_count(), 1);
}

TEST(TrialRunnerRun, ADurationOfPartOfAMillisecondIsTheWindowsWidthExactly) {
  Machine machine;
  machine.workers_that_exit_at_once(2);
  Candidate candidate = a_candidate();
  candidate.workloads = {"sort", "fft"};
  candidate.threads = 2;
  candidate.duration_ns = 1'500'000;  // not whole milliseconds
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  const statistics::Window window = outcome.value().signature.window;
  EXPECT_EQ(window.end_ns - window.start_ns, 1'500'000) << "exactly the duration, not rounded";
  EXPECT_EQ(machine.syscalls.fork_count(), 2) << "one worker per thread";
  EXPECT_FALSE(outcome.value().signature.metrics.empty());
}

TEST(TrialRunnerRun, ThreadsAreDealtTheWorkloadsInTurn) {
  Machine machine;
  machine.workers_that_exit_at_once(5);
  Candidate candidate = a_candidate();
  candidate.workloads = {"sort", "fft"};
  candidate.threads = 5;
  candidate.cpus = {0, 0};
  auto runner = machine.runner();
  ASSERT_TRUE(runner->run(candidate).has_value());
  EXPECT_EQ(machine.syscalls.fork_count(), 5);
}

TEST(TrialRunnerRun, ALogLineSaysHowEachTrialWentAndWithoutALogNothingIsSaid) {
  Machine machine;
  machine.workers_that_exit_at_once(1);
  std::ostringstream log;
  machine.options.log = &log;
  auto runner = machine.runner();
  ASSERT_TRUE(runner->run(a_candidate()).has_value());
  EXPECT_TRUE(contains(log.str(), "trial probe: incomplete, not settled after 0 ns\n"))
      << log.str();
  EXPECT_TRUE(contains(log.str(), "worker")) << "the controller's own words are there too";

  Machine quiet;
  quiet.workers_that_exit_at_once(1);
  auto silent = quiet.runner();
  ASSERT_TRUE(silent->run(a_candidate()).has_value());
}

TEST(TrialRunnerRun, AForkTheKernelRefusesIsAKRunThatSaysSo) {
  Machine machine;
  machine.syscalls.script_forks({{0, EAGAIN}});
  auto runner = machine.runner();
  const auto outcome = runner->run(a_candidate());
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(outcome.error().detail, "fork")) << outcome.error().detail;
  EXPECT_FALSE(runner->worker_exit_code().has_value());
}

TEST(TrialRunnerRun, AWorkerWhoseExecFailedIsThisProcessAndItsExitCodeIsKept) {
  Machine machine;
  machine.syscalls.script_forks({{0, 0}});  // This process is the worker; its exec will fail.
  machine.syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  auto runner = machine.runner();
  const auto outcome = runner->run(a_candidate());
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(outcome.error().detail, "must exit with 120")) << outcome.error().detail;
  EXPECT_EQ(runner->worker_exit_code(), 120);
}

TEST(TrialRunnerRun, TelemetryThatStopsMidTrialIsAKRunNotAWindowOfNothing) {
  Machine machine;
  machine.options.sources = crowd;
  machine.workers_that_exit_at_once(1);
  auto runner = machine.runner();
  const auto outcome = runner->run(a_candidate());
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(outcome.error().detail, "telemetry stopped during the trial"))
      << outcome.error().detail;
}

TEST(TrialRunnerRun, ARigTheTimelineRefusesAtTheTrialIsAKRun) {
  Machine machine;
  auto runner = machine.runner();
  // The sources are made again for the trial: these twin the second time.
  static int made = 0;
  made = 0;
  struct Once {
    static std::vector<std::unique_ptr<telemetry::Source>> make(platform::Syscalls& s,
                                                                platform::FileSystem& f,
                                                                const MachineRoots& r) {
      return ++made == 1 ? make_sources(s, f, r) : twins(s, f, r);
    }
  };
  Machine second;
  second.options.sources = &Once::make;
  auto stateful = second.runner();
  const auto outcome = stateful->run(a_candidate());
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(outcome.error().detail, "cannot set up telemetry: duplicate series"));
  EXPECT_EQ(second.syscalls.fork_count(), 0);
}

TEST(TrialRunnerRun, AResultThatCannotBeComputedIsAKRunAndSoIsAWindowThatCannotBeSummarised) {
  {
    Machine machine;
    machine.workers_that_exit_at_once(1);
    const FailingSummarizer failing(1);
    machine.options.summarizer = &failing;
    auto runner = machine.runner();
    const auto outcome = runner->run(a_candidate());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
    EXPECT_TRUE(contains(outcome.error().detail, "cannot compute the trial's result"))
        << outcome.error().detail;
  }
  {
    Machine machine;
    machine.workers_that_exit_at_once(1);
    const FailingSummarizer failing(2);
    machine.options.summarizer = &failing;
    auto runner = machine.runner();
    const auto outcome = runner->run(a_candidate());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
    EXPECT_TRUE(contains(outcome.error().detail,
                         "cannot summarise the load window: empty or "
                         "inverted window: scripted"))
        << outcome.error().detail;
  }
}

// --- the settle phase --------------------------------------------------------------------------

TEST(TrialRunnerSettle, AGaugeThatHoldsStillSettlesOnceTheWindowIsFull) {
  Machine machine;
  machine.options.sources = flat_level;
  machine.workers_that_exit_at_once(1);
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 500 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_TRUE(outcome.value().settled);
  EXPECT_GE(outcome.value().settle_ns, 30 * kMs) << "never before the window is full";
  EXPECT_LE(outcome.value().settle_ns, 500 * kMs);
}

TEST(TrialRunnerSettle, AGaugeThatKeepsRisingRunsOutTheTimeAndSaysItDidNotSettle) {
  Machine machine;
  machine.options.sources = rising_level;
  machine.workers_that_exit_at_once(1);
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_FALSE(outcome.value().settled);
  EXPECT_EQ(outcome.value().settle_ns, 100 * kMs) << "the whole of max_ns";
}

TEST(TrialRunnerSettle, ASlopeWithinTheBoundInEitherDirectionCounts) {
  Machine machine;
  machine.options.sources = rising_level;  // 5 degrees per 10 ms: 500 per second
  machine.workers_that_exit_at_once(1);
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 1.0e6, 30 * kMs, 100 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_TRUE(outcome.value().settled);
}

TEST(TrialRunnerSettle, ASensorThatDroppedOutCannotSettleAndIsNotCalledSettled) {
  Machine machine;
  machine.options.sources = blind_level;
  machine.workers_that_exit_at_once(1);
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_FALSE(outcome.value().settled);
  EXPECT_EQ(outcome.value().settle_ns, 100 * kMs);
}

TEST(TrialRunnerSettle, ASettleWindowTheSummarizerRefusesIsNotSettled) {
  Machine machine;
  machine.options.sources = flat_level;
  machine.workers_that_exit_at_once(1);
  const FailingSummarizer failing(1);  // the first window asked about
  machine.options.summarizer = &failing;
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_TRUE(outcome.value().settled) << "the next window was answered";
}

TEST(TrialRunnerSettle, AClockThatCannotBeReadIsAKRunAtTheStartAndInTheLoop) {
  for (const int failing_read : {1, 2}) {
    Machine machine;
    machine.options.sources = flat_level;
    machine.syscalls.fail_clock_on_read(kClockMonotonic, failing_read, EINVAL);
    machine.workers_that_exit_at_once(1);
    Candidate candidate = a_candidate();
    candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
    // The probe of the runner reads no clock; the first reads are the settle phase's.
    auto runner = machine.runner();
    const auto outcome = runner->run(candidate);
    ASSERT_FALSE(outcome.has_value()) << failing_read;
    EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
    EXPECT_TRUE(contains(outcome.error().detail, "settle: clock_gettime"))
        << outcome.error().detail;
    EXPECT_EQ(machine.syscalls.fork_count(), 0) << "nothing starts from an unsettled machine";
  }
}

TEST(TrialRunnerSettle, ARoundTheTimelineRefusesEndsTheTrial) {
  Machine machine;
  machine.options.sources = flat_level;
  machine.syscalls.advance_clocks_per_read(0);  // time stands still: the second round is not later
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
  auto runner = machine.runner();
  const auto outcome = runner->run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
  EXPECT_TRUE(contains(outcome.error().detail, "settle: sampling refused: out of order"))
      << outcome.error().detail;
}

TEST(TrialRunnerSettle, AWaitThatFailsIsAKRunButOneASignalCutShortIsNot) {
  Candidate candidate = a_candidate();
  candidate.settle = Settle{"level.reading", 0.05, 30 * kMs, 100 * kMs};
  {
    Machine machine;
    machine.options.sources = flat_level;
    machine.syscalls.fail_poll_on(1, EINVAL);
    auto runner = machine.runner();
    const auto outcome = runner->run(candidate);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().fault, TrialFault::kRun);
    EXPECT_TRUE(contains(outcome.error().detail, "settle: poll")) << outcome.error().detail;
  }
  {
    Machine machine;
    machine.options.sources = flat_level;
    machine.workers_that_exit_at_once(1);
    machine.syscalls.fail_poll_on(1, EINTR);
    auto runner = machine.runner();
    const auto outcome = runner->run(candidate);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome.value().settled);
  }
}

TEST(TrialRunnerSettle, TheWaitIsAQuarterOfTheWindowBetweenAMillisecondAndTheSampleInterval) {
  struct Case {
    std::int64_t window_ns;
    std::int64_t max_ns;
    int first_wait_ms;
    const char* why;
  };
  for (const Case& each :
       {Case{2 * kMs, 400 * kMs, 1, "a quarter is under the floor of a millisecond"},
        Case{6 * kMs, 400 * kMs, 2, "1.5 ms is rounded up to a whole millisecond"},
        Case{400 * kMs, 4000 * kMs, 30, "never longer than the sample interval"},
        Case{400 * kMs, 15 * kMs, 5, "never past the end of the phase"}}) {
    Machine machine;
    machine.options.sources = rising_level;
    machine.workers_that_exit_at_once(1);
    Candidate candidate = a_candidate();
    candidate.settle = Settle{"level.reading", 0.05, each.window_ns, each.max_ns};
    auto runner = machine.runner();
    ASSERT_TRUE(runner->run(candidate).has_value()) << each.why;
    const auto& waits = machine.syscalls.poll_timeouts();
    ASSERT_FALSE(waits.empty()) << each.why;
    EXPECT_EQ(waits.front(), each.first_wait_ms) << each.why;
  }
}

// --- the contract -----------------------------------------------------------------------------

/// The runner with everything it stands on, behind the contract's interface.
class OwnedRunner final : public explore::TrialRunner {
 public:
  OwnedRunner() {
    machine_.options.sources = flat_level;
    machine_.syscalls.script_forks({{1001, 0}});
    machine_.syscalls.script_nonblocking_reaps({{platform::Reaped{1001, 0}, 0}});
    runner_ = machine_.runner();
  }
  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const override {
    return runner_->capabilities();
  }
  core::Result<explore::TrialOutcome, explore::TrialError> run(
      const Candidate& candidate) override {
    return runner_->run(candidate);
  }

 private:
  Machine machine_;
  std::unique_ptr<ControllerTrialRunner> runner_;
};

struct ControllerRunnerOverTheFake {
  static constexpr const char* kPresentGauge = "level.reading";
  static std::unique_ptr<explore::TrialRunner> make() { return std::make_unique<OwnedRunner>(); }
  static Candidate candidate() { return a_candidate(); }
};

}  // namespace
}  // namespace loadforge::cli

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerRunnerOverTheFake, TrialRunnerContract,
                               ::testing::Types<cli::ControllerRunnerOverTheFake>);
}  // namespace loadforge::contracts
