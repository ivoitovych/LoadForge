// SPDX-License-Identifier: GPL-3.0-or-later
//
// The controller's sampling (orchestration.hpp, RunOptions::sampler): when it
// takes a round, what it marks, and what it does when the timeline refuses --
// over the fake seam and this module's own mocks of the sampler and the
// timeline (contract_mocks_test.cpp runs the providers' suites against them).

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/telemetry_mocks.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::Reaped;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;
constexpr int kRecordWrite0 = 201;
constexpr int kControlRead0 = 202;
constexpr int kRecordWrite1 = 205;
constexpr int kControlRead1 = 206;

class Recorder final : public Reporter {
 public:
  void run_starting(const RunPlan& /*plan*/) override {
    for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
      syscalls->worker_holds_end(fd);
    }
  }
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    events.emplace_back(line);
    if (line.substr(0, 8) == "deadline") {
      // Both workers finish at the deadline.
      for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
        syscalls->worker_releases_end(fd);
      }
      syscalls->script_nonblocking_reaps({{Reaped{1001, 0}, 0}, {Reaped{1002, 0}, 0}});
    }
  }
  void run_finished(const RunReport& /*report*/) override {}
  FakeSyscalls* syscalls = nullptr;
  std::vector<std::string> events;
};

struct Sampled {
  FakeSyscalls syscalls;
  Recorder reporter;
  config::Config config;
  RunOptions options;
  mocks::MockTimeline timeline;
  mocks::PointSampler sampler;

  Sampled()
      : sampler(timeline,
                telemetry::Metric{"mock.count", telemetry::Unit::kCount, telemetry::Kind::kCounter,
                                  telemetry::Scope::kMachine, 0, std::nullopt, "mock"}) {
    reporter.syscalls = &syscalls;
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{100};
    config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
    config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
    options.executable = "/opt/loadforge";
    options.interval_ns = 10 * kMs;
    options.stale_after_ns = 1000 * kMs;
    options.grace = Grace{20 * kMs, 20 * kMs};
    options.sampler = &sampler;
    options.timeline = &timeline;
    options.sample_interval_ns = 30 * kMs;
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.script_forks({{1001, 0}, {1002, 0}});
    EXPECT_TRUE(sampler.probe().has_value());
  }

  core::Result<RunReport, ControllerError> go() {
    Controller controller(syscalls, reporter);
    return controller.run(config, options);
  }
};

bool mentions(const Recorder& reporter, std::string_view text) {
  return std::ranges::any_of(reporter.events, [&](const std::string& event) {
    return event.find(text) != std::string::npos;
  });
}

TEST(OrchestrationTelemetryTest,
     ARoundIsTakenBeforeTheForkAtEveryIntervalAtTheDeadlineAndAfterTheReap) {
  Sampled run;
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  const SamplingReport& sampling = result.value().sampling;
  EXPECT_TRUE(sampling.enabled);
  EXPECT_EQ(sampling.interval_ns, 30 * kMs);
  EXPECT_FALSE(sampling.fault.has_value());
  ASSERT_GE(run.sampler.rounds_asked.size(), 5U);
  // Strictly later every time: the timeline refuses anything else.
  for (std::size_t at = 1; at < run.sampler.rounds_asked.size(); ++at) {
    EXPECT_GT(run.sampler.rounds_asked[at], run.sampler.rounds_asked[at - 1]);
  }
  // Between ordinary rounds, at least the interval; the forced ones (the
  // deadline's, the last) may come sooner.
  EXPECT_EQ(sampling.rounds, run.sampler.rounds_asked.size());
  EXPECT_EQ(sampling.points, run.sampler.rounds_asked.size());
  EXPECT_EQ(sampling.first_round_ns, run.sampler.rounds_asked.front());
  EXPECT_EQ(sampling.last_round_ns, run.sampler.rounds_asked.back());
  EXPECT_GT(sampling.cost_ns, 0) << "the observer is measured";
  EXPECT_EQ(run.timeline.points(0).size(), sampling.rounds);
}

TEST(OrchestrationTelemetryTest, TheBaselineIsTakenBeforeAnyWorkerExists) {
  Sampled run;
  run.reporter.syscalls = &run.syscalls;
  int forks_at_baseline = -1;
  // The first round is the first thing the sampler is asked; the fake counts
  // forks, so a round taken before the fork sees none.
  class Watching final : public telemetry::Sampler {
   public:
    Watching(Sampled& run, mocks::PointSampler& inner) : run_(&run), inner_(&inner) {}
    core::Result<telemetry::CapabilityReport, telemetry::TimelineError> probe() override {
      return inner_->probe();
    }
    core::Result<std::size_t, telemetry::TimelineError> round(std::int64_t now_ns) override {
      if (forks_at_first < 0) {
        forks_at_first = run_->syscalls.fork_count();
      }
      return inner_->round(now_ns);
    }
    int forks_at_first = -1;

   private:
    Sampled* run_;
    mocks::PointSampler* inner_;
  } watching(run, run.sampler);
  run.options.sampler = &watching;
  ASSERT_TRUE(run.go().has_value());
  forks_at_baseline = watching.forks_at_first;
  EXPECT_EQ(forks_at_baseline, 0);
}

TEST(OrchestrationTelemetryTest, TheRunIsMarkedAsAPhaseAndSoIsTheStop) {
  Sampled run;
  ASSERT_TRUE(run.go().has_value());
  const auto phases = run.timeline.phases();
  ASSERT_EQ(phases.size(), 2U);
  EXPECT_EQ(phases[0].label, "run");
  EXPECT_EQ(phases[1].label, "stop");
  // The mark and the round that closes the phase before it are one instant.
  const auto points = run.timeline.points(0);
  EXPECT_TRUE(std::ranges::any_of(points, [&](const telemetry::Point& point) {
    return point.timestamp_ns == phases[1].start_ns;
  }));
  EXPECT_EQ(phases[0].start_ns, points.front().timestamp_ns);
}

TEST(OrchestrationTelemetryTest, EveryReapIsTimedOnTheTimelinesClock) {
  Sampled run;
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  const SamplingReport& sampling = result.value().sampling;
  ASSERT_EQ(sampling.reaps.size(), 2U);
  EXPECT_EQ(sampling.reaps[0].slot, 0U);
  EXPECT_EQ(sampling.reaps[1].slot, 1U);
  for (const Reap& reap : sampling.reaps) {
    EXPECT_GT(reap.at_ns, sampling.first_round_ns);
    EXPECT_LE(reap.at_ns, sampling.last_round_ns);
  }
}

TEST(OrchestrationTelemetryTest, ARunWithoutASamplerSamplesNothingAndRecordsNoReap) {
  Sampled run;
  run.options.sampler = nullptr;
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value().sampling.enabled);
  EXPECT_EQ(result.value().sampling.rounds, 0U);
  EXPECT_TRUE(result.value().sampling.reaps.empty());
  EXPECT_TRUE(run.sampler.rounds_asked.empty());
  EXPECT_TRUE(run.timeline.phases().empty()) << "no sampler, no phases";
}

TEST(OrchestrationTelemetryTest, ASamplerWithoutATimelineIsStillSampledAndMarksNothing) {
  Sampled run;
  run.options.timeline = nullptr;
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result.value().sampling.rounds, 0U);
  EXPECT_TRUE(run.timeline.phases().empty());
}

TEST(OrchestrationTelemetryTest, ARoundTheTimelineRefusesEndsSamplingAndIsReportedNeverTheRun) {
  Sampled run;
  run.sampler.fail_from_round = 3;
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << "a full timeline does not end a run";
  const SamplingReport& sampling = result.value().sampling;
  EXPECT_EQ(sampling.rounds, 2U);
  ASSERT_TRUE(sampling.fault.has_value());
  EXPECT_EQ(*sampling.fault, "sampling stopped: capacity reached (full)");
  EXPECT_EQ(run.sampler.rounds_asked.size(), 3U) << "no round is asked of a sampler that failed";
  EXPECT_TRUE(mentions(run.reporter, "telemetry: sampling stopped: capacity reached (full)"));
  EXPECT_EQ(sampling.last_round_ns, run.sampler.rounds_asked[1]);
}

TEST(OrchestrationTelemetryTest, ARefusalWithNoReasonIsReportedWithoutEmptyBrackets) {
  Sampled run;
  mocks::MockSampler quiet({}, run.timeline);
  quiet.fail_round = telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, ""};
  run.options.sampler = &quiet;
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().sampling.fault.has_value());
  EXPECT_EQ(*result.value().sampling.fault, "sampling stopped: out of order");
}

TEST(OrchestrationTelemetryTest, APhaseTheTimelineRefusesIsReportedAndOnlyTheFirstFaultIsKept) {
  Sampled run;
  run.timeline.refuse_phases();
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  const SamplingReport& sampling = result.value().sampling;
  ASSERT_TRUE(sampling.fault.has_value());
  EXPECT_EQ(*sampling.fault, "phase \"run\" refused: out of order")
      << "the stop's refusal, later, does not replace it";
  EXPECT_TRUE(mentions(run.reporter, "phase \"stop\" refused: out of order")) << "but it is told";
  EXPECT_GT(sampling.rounds, 2U) << "a refused phase does not stop the sampling";
}

TEST(OrchestrationTelemetryTest, AClockThatCannotBeReadAfterARoundLeavesTheCostALowerBound) {
  Sampled run;
  // now() reads three clocks and is called, in order: at the start (1), after
  // the baseline round to time it (2), and on the first tick (3).
  run.syscalls.fail_clock_on_read(CLOCK_MONOTONIC_RAW, 2, EIO);
  auto result = run.go();
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value().sampling.fault.has_value());
  EXPECT_GT(result.value().sampling.rounds, 2U);
  EXPECT_GT(result.value().sampling.cost_ns, 0) << "the later rounds were timed";
}

TEST(OrchestrationTelemetryTest, ACostThatCouldNotGoBackwardsIsNeverNegative) {
  // The clock after a round is not earlier than the round's own time unless the
  // fake says so; a clock stepped back leaves the cost unchanged, not negative.
  Sampled run;
  run.syscalls.jump_clock_on_read(CLOCK_MONOTONIC, 2, platform::TimeSpec{0, 0});
  auto result = run.go();
  EXPECT_TRUE(!result.has_value() || result.value().sampling.cost_ns >= 0);
}

}  // namespace
}  // namespace loadforge::controller
