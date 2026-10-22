// SPDX-License-Identifier: GPL-3.0-or-later
//
// The temperature ceiling in the controller (limits.max_temperature_c): a
// reading at or above it stops every worker as the deadline does, a ceiling
// nothing can read refuses the run before any worker exists, and what the run
// says of it -- the banner, the events, the report, the journal -- over the
// fake seam and this module's own mocks of the sampler, the timeline and a
// thermal source (telemetry_mocks.hpp).

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "controller/temperature.hpp"
#include "support/fake_syscalls.hpp"
#include "topology/source.hpp"
#include "unit/controller/journal_model.hpp"
#include "unit/controller/telemetry_mocks.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::Reaped;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;
constexpr pid_t kFirstPid = 1001;
constexpr pid_t kSecondPid = 1002;
constexpr int kRecordWrite0 = 201;
constexpr int kControlRead0 = 202;
constexpr int kControlWrite0 = 203;
constexpr int kRecordWrite1 = 205;
constexpr int kControlRead1 = 206;

bool starts_with(std::string_view line, std::string_view prefix) {
  return line.substr(0, prefix.size()) == prefix;
}

/// Plays the workers from inside the reporter's callbacks: they hold their ends
/// from the start, and leave (or not) when the controller says it is stopping them.
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
    if (starts_with(line, "temperature ceiling")) {
      stop_frames = syscalls->pipe_contents(kControlWrite0).size();
    }
    if (leave_when && leave_when(line)) {
      leave();
    }
    if (on_event) {
      on_event(std::string{line});
    }
  }
  void run_finished(const RunReport& report) override { reports.push_back(report); }
  void isolation_finished(const RunReport& /*report*/) override {}

  void leave() {
    for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
      syscalls->worker_releases_end(fd);
    }
    syscalls->child_exits(kFirstPid);
    syscalls->child_exits(kSecondPid);
    syscalls->script_nonblocking_reaps({{Reaped{kFirstPid, 0}, 0}, {Reaped{kSecondPid, 0}, 0}});
  }

  [[nodiscard]] bool told(std::string_view part) const {
    return std::ranges::any_of(events, [part](const std::string& event) {
      return event.find(part) != std::string::npos;
    });
  }

  FakeSyscalls* syscalls = nullptr;
  std::function<bool(std::string_view)> leave_when;
  std::function<void(const std::string&)> on_event;
  std::vector<RunPlan> plans;
  std::vector<std::string> events;
  std::vector<std::int64_t> elapsed;
  std::vector<RunReport> reports;
  std::size_t stop_frames = 0;
};

telemetry::Metric celsius(std::string name, telemetry::Scope scope, std::uint32_t index) {
  return telemetry::Metric{std::move(name), telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                           scope,           index,                     std::nullopt,
                           "hwmon:mock"};
}

telemetry::Reading reads(double value) { return telemetry::Gauge{value}; }

telemetry::Reading gone() {
  return telemetry::Unavailable{topology::Availability::kVanished, "/sys/x",
                                "the sensor behind hwmon:mock disappeared", "re-run when stable"};
}

/// `count` rounds of the same readings, as a script.
std::vector<std::vector<telemetry::Reading>> repeated(std::size_t count,
                                                      std::vector<telemetry::Reading> round) {
  return std::vector<std::vector<telemetry::Reading>>(count, std::move(round));
}

std::vector<std::vector<telemetry::Reading>> then(
    std::vector<std::vector<telemetry::Reading>> first,
    const std::vector<std::vector<telemetry::Reading>>& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

/// A run over a thermal source that reads what the script says, round by round
/// (round 0 is the baseline, taken before the first fork; the last repeats).
struct Heated {
  FakeSyscalls syscalls;
  Plant reporter;
  config::Config config;
  RunOptions options;
  mocks::MockTimeline timeline;
  std::unique_ptr<mocks::MockSampler> sampler;

  Heated(std::vector<telemetry::Metric> metrics,
         std::vector<std::vector<telemetry::Reading>> rounds, std::int64_t ceiling = 85,
         bool present = true) {
    std::vector<telemetry::MetricStatus> statuses;
    for (telemetry::Metric& metric : metrics) {
      telemetry::MetricStatus status;
      status.metric = std::move(metric);
      if (!present) {
        status.unavailable = telemetry::Unavailable{topology::Availability::kAbsent, "/sys/class",
                                                    "no hwmon chip", "nothing can be done"};
      }
      statuses.push_back(std::move(status));
    }
    std::vector<std::unique_ptr<telemetry::Source>> sources;
    sources.push_back(
        std::make_unique<mocks::MockSource>("thermal", std::move(statuses), std::move(rounds)));
    sampler = std::make_unique<mocks::MockSampler>(std::move(sources), timeline);
    EXPECT_TRUE(sampler->probe().has_value());

    reporter.syscalls = &syscalls;
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{5000};
    config.limits.max_temperature_c = ceiling;
    config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
    config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
    options.executable = "/opt/loadforge";
    options.interval_ns = 10 * kMs;
    options.stale_after_ns = 100'000 * kMs;
    options.grace = Grace{20 * kMs, 20 * kMs};
    options.sampler = sampler.get();
    options.timeline = &timeline;
    options.view = &timeline;
    options.sample_interval_ns = 30 * kMs;
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.script_forks({{kFirstPid, 0}, {kSecondPid, 0}});
    // The workers leave once the controller says it is stopping them.
    reporter.leave_when = [](std::string_view line) {
      return starts_with(line, "temperature ceiling") || starts_with(line, "deadline");
    };
  }

  core::Result<RunReport, ControllerError> go() {
    Controller controller(syscalls, reporter);
    return controller.run(config, options);
  }
};

const std::vector<telemetry::Metric> kPackage{
    celsius("thermal.package", telemetry::Scope::kPackage, 0)};

// --- a reading below, at and above the ceiling -----------------------------------------

TEST(OrchestrationTemperatureTest, ARunBelowTheCeilingRunsToItsDeadlineAndSaysItWatchesTheSensors) {
  Heated run(kPackage, repeated(1, {reads(84.9)}));
  run.config.run.duration = core::Duration{100};
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().temperature_stop.has_value());
  EXPECT_TRUE(run.reporter.told("deadline reached: stop requested"));
  EXPECT_FALSE(run.reporter.told("temperature"));
  ASSERT_EQ(run.reporter.plans.size(), 1U);
  const std::vector<LimitStatus>& limits = run.reporter.plans[0].limits;
  ASSERT_EQ(limits.size(), 2U);
  EXPECT_EQ(limits[1], (LimitStatus{"temperature", true,
                                    "stops the run at 85 C; watching thermal.package 0"}));
}

TEST(OrchestrationTemperatureTest, AReadingAtTheCeilingStopsEveryWorkerBeforeTheDeadline) {
  Heated run(kPackage, then(repeated(3, {reads(60)}), repeated(1, {reads(85)})));
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const RunReport& report = result.value();
  ASSERT_TRUE(report.temperature_stop.has_value());
  const TemperatureStop& stop = *report.temperature_stop;
  EXPECT_EQ(stop.cause, StopCause::kReached);
  EXPECT_EQ(stop.sensor, "thermal.package 0 (hwmon:mock)");
  EXPECT_EQ(stop.celsius, 85.0);
  EXPECT_EQ(stop.ceiling_c, 85);
  EXPECT_GT(stop.at_ns, 0);
  EXPECT_LT(stop.at_ns, 5000 * kMs) << "long before the deadline";
  EXPECT_FALSE(run.reporter.told("deadline reached"));
  EXPECT_TRUE(run.reporter.told("temperature ceiling: thermal.package 0 (hwmon:mock) read 85.0 C, "
                                "at or above the ceiling of 85 C, "));
  EXPECT_TRUE(run.reporter.told("s into the run: stop requested"));
  // Every worker was asked, exactly as at the deadline: a Stop on each control pipe.
  ASSERT_EQ(report.workers.size(), 2U);
  EXPECT_EQ(report.workers[0].commands.sent, 1U);
  EXPECT_EQ(report.workers[1].commands.sent, 1U);
  EXPECT_EQ(report.workers[0].stop, "stop requested");
  EXPECT_GT(run.reporter.stop_frames, 0U) << "the Stop was on the pipe when the event was told";
  // The timeline's phases: the run, and its stop where the ceiling ended it.
  ASSERT_EQ(run.timeline.phases().size(), 2U);
  EXPECT_EQ(run.timeline.phases()[0].label, "run");
  EXPECT_EQ(run.timeline.phases()[1].label, "stop");
  EXPECT_EQ(report.sampling.fault, std::nullopt);
}

TEST(OrchestrationTemperatureTest, AReadingAboveTheCeilingStopsTheRunToo) {
  Heated run(kPackage, then(repeated(2, {reads(70)}), repeated(1, {reads(91.5)})));
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  EXPECT_EQ(result.value().temperature_stop->celsius, 91.5);
}

TEST(OrchestrationTemperatureTest, WithNoCeilingATemperatureIsOnlyRecorded) {
  Heated run(kPackage, repeated(1, {reads(120)}));
  run.config.limits.max_temperature_c.reset();
  run.config.run.duration = core::Duration{100};
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().temperature_stop.has_value());
  EXPECT_FALSE(run.reporter.told("temperature"));
  EXPECT_EQ(run.reporter.plans[0].limits.size(), 1U) << "no temperature limit is stated";
}

TEST(OrchestrationTemperatureTest, OfSeveralSensorsTheOneThatCrossesIsNamedAndTheBannerListsAll) {
  const std::vector<telemetry::Metric> sensors{
      celsius("thermal.package", telemetry::Scope::kPackage, 0),
      celsius("thermal.core", telemetry::Scope::kCore, 0),
      celsius("thermal.core", telemetry::Scope::kCore, 1)};
  Heated run(sensors,
             then(repeated(3, {reads(60), reads(61), reads(62)}),
                  repeated(1, {reads(70), reads(71), reads(90.25)})));
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  EXPECT_EQ(result.value().temperature_stop->sensor, "thermal.core 1 (hwmon:mock)");
  EXPECT_EQ(result.value().temperature_stop->celsius, 90.25);
  EXPECT_EQ(run.reporter.plans[0].limits[1].note,
            "stops the run at 85 C; watching thermal.package 0, thermal.core x2");
}

// --- a source that drops out mid-run, and no sensor at all -------------------------------

TEST(OrchestrationTemperatureTest, ASensorThatDropsOutIsToldAndTheOthersKeepTheCeiling) {
  const std::vector<telemetry::Metric> sensors{
      celsius("thermal.package", telemetry::Scope::kPackage, 0),
      celsius("thermal.core", telemetry::Scope::kCore, 0)};
  auto rounds = then(repeated(2, {reads(60), reads(60)}), repeated(3, {gone(), reads(61)}));
  rounds = then(std::move(rounds), repeated(2, {reads(62), reads(62)}));
  rounds = then(std::move(rounds), repeated(1, {gone(), reads(88)}));
  Heated run(sensors, std::move(rounds));
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_TRUE(run.reporter.told("temperature: thermal.package 0 (hwmon:mock) can no longer be "
                                "read: the sensor behind hwmon:mock disappeared"));
  EXPECT_TRUE(run.reporter.told("temperature: thermal.package 0 (hwmon:mock) can be read again"));
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  EXPECT_EQ(result.value().temperature_stop->sensor, "thermal.core 0 (hwmon:mock)")
      << "the sensor that remained crossed while the other was gone";
}

TEST(OrchestrationTemperatureTest, WhenEverySensorIsGoneForTooLongTheRunIsStoppedAsUnwatched) {
  Heated run(kPackage, then(repeated(2, {reads(60)}), repeated(1, {gone()})));
  run.options.temperature_blind_after_ns = 50 * kMs;
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  const TemperatureStop& stop = *result.value().temperature_stop;
  EXPECT_EQ(stop.cause, StopCause::kUnwatched);
  EXPECT_EQ(stop.detail, "the sensor behind hwmon:mock disappeared");
  EXPECT_GT(stop.blind_ns, 50 * kMs);
  EXPECT_LT(stop.at_ns, 5000 * kMs);
  EXPECT_TRUE(run.reporter.told("temperature ceiling: no temperature could be read for "));
  EXPECT_TRUE(run.reporter.told("can no longer be enforced"));
  EXPECT_EQ(result.value().workers[0].commands.sent, 1U) << "the workers were stopped";
}

TEST(OrchestrationTemperatureTest, WhenSamplingEndsMidRunTheCeilingCanNoLongerBeHeldAndTheRunStops) {
  mocks::MockTimeline timeline;
  mocks::PointSampler sampler(timeline, celsius("thermal.package", telemetry::Scope::kPackage, 0));
  ASSERT_TRUE(sampler.probe().has_value());
  sampler.fail_from_round = 4;  // The timeline is full from the fourth round.
  Heated run(kPackage, repeated(1, {reads(40)}));
  run.options.sampler = &sampler;
  run.options.timeline = &timeline;
  run.options.view = &timeline;
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  EXPECT_EQ(result.value().temperature_stop->cause, StopCause::kUnwatched);
  EXPECT_EQ(result.value().temperature_stop->detail, "sampling stopped: capacity reached (full)");
  EXPECT_TRUE(run.reporter.told("telemetry: sampling stopped: capacity reached (full)"));
  EXPECT_TRUE(run.reporter.told("temperature ceiling: "));
}

TEST(OrchestrationTemperatureTest, ACeilingWithNoSensorAtAllRefusesTheRunBeforeAnyWorkerExists) {
  Heated run(kPackage, repeated(1, {reads(40)}), 85, false);
  run.options.temperature_unread = "thermal.package: no hwmon chip; nothing can be done";
  auto result = run.go();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "temperature");
  EXPECT_EQ(result.error().detail,
            "the temperature ceiling is 85 C and no temperature can be read, so it "
            "could not be enforced: thermal.package: no hwmon chip; nothing can be done; remove "
            "the ceiling to run without one");
  EXPECT_EQ(run.syscalls.fork_count(), 0);
  EXPECT_TRUE(run.reporter.plans.empty()) << "refused before the banner";
  EXPECT_TRUE(run.syscalls.all_descriptors_closed());
}

TEST(OrchestrationTemperatureTest, WithoutAReasonTheRefusalSaysNoSensorWasOffered) {
  Heated run(kPackage, repeated(1, {reads(40)}), 85, false);
  auto result = run.go();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().detail.find("could not be enforced: no temperature sensor was offered; "),
            std::string::npos)
      << result.error().detail;
}

TEST(OrchestrationTemperatureTest, SensorsThatAllFailTheBaselineRefuseTheRunWithTheirReason) {
  Heated run(kPackage, repeated(1, {gone()}));
  auto result = run.go();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "temperature");
  EXPECT_NE(result.error().detail.find("none of the 1 temperature sensor(s) answered: the sensor "
                                       "behind hwmon:mock disappeared"),
            std::string::npos)
      << result.error().detail;
  EXPECT_EQ(run.syscalls.fork_count(), 0);
}

TEST(OrchestrationTemperatureTest, AMachineAlreadyAtTheCeilingIsRefusedBeforeAnyWorkerStarts) {
  Heated run(kPackage, repeated(1, {reads(86)}));
  auto result = run.go();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "temperature");
  EXPECT_NE(result.error().detail.find("already at its ceiling, before any worker started: "
                                       "thermal.package 0 (hwmon:mock) read 86.0 C"),
            std::string::npos)
      << result.error().detail;
  EXPECT_EQ(run.syscalls.fork_count(), 0);
}

TEST(OrchestrationTemperatureTest, ACeilingWithNothingToReadWithRefusesToo) {
  {
    Heated run(kPackage, repeated(1, {reads(40)}));
    run.options.view = nullptr;
    run.options.temperature_unread = "telemetry is off";
    auto result = run.go();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, "temperature");
    EXPECT_NE(result.error().detail.find("could not be enforced: telemetry is off; "),
              std::string::npos);
    EXPECT_EQ(run.syscalls.fork_count(), 0);
  }
  {
    Heated run(kPackage, repeated(1, {reads(40)}));
    run.options.sampler = nullptr;
    auto result = run.go();
    ASSERT_FALSE(result.has_value()) << "a view with no sampler behind it reads nothing";
    EXPECT_EQ(result.error().stage, "temperature");
  }
  {
    Heated run(kPackage, repeated(1, {reads(40)}));
    run.sampler->fail_round = telemetry::TimelineError{telemetry::TimelineFault::kCapacity, "full"};
    auto result = run.go();
    ASSERT_FALSE(result.has_value()) << "a baseline round the timeline refused is no reading";
    EXPECT_EQ(result.error().stage, "temperature");
  }
}

// --- how the run ends ----------------------------------------------------------------

TEST(OrchestrationTemperatureTest, WorkersThatIgnoreTheStopAreTerminatedThenKilledAsAtTheDeadline) {
  Heated run(kPackage, then(repeated(2, {reads(60)}), repeated(1, {reads(95)})));
  run.reporter.leave_when = [](std::string_view line) { return line == "worker 1: sent signal 9"; };
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  const std::vector<std::pair<pid_t, int>> sent = run.syscalls.signals_sent();
  ASSERT_GE(sent.size(), 4U);
  EXPECT_EQ(sent[0], (std::pair<pid_t, int>{kFirstPid, SIGTERM}));
  EXPECT_EQ(sent[1], (std::pair<pid_t, int>{kSecondPid, SIGTERM}));
  EXPECT_NE(std::ranges::find(sent, std::pair<pid_t, int>{kFirstPid, SIGKILL}), sent.end());
  EXPECT_NE(std::ranges::find(sent, std::pair<pid_t, int>{kSecondPid, SIGKILL}), sent.end());
  EXPECT_EQ(result.value().workers[0].stop, "SIGKILL sent");
}

TEST(OrchestrationTemperatureTest, ACrossingAfterTheDeadlineIsStillTheRunsAndAsksNothingTwice) {
  Heated run(kPackage, then(repeated(10, {reads(60)}), repeated(1, {reads(97)})));
  run.config.run.duration = core::Duration{50};
  run.options.grace = Grace{5000 * kMs, 5000 * kMs};
  run.reporter.leave_when = [](std::string_view line) {
    return starts_with(line, "temperature ceiling");
  };
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().temperature_stop.has_value());
  const auto deadline = std::ranges::find_if(run.reporter.events, [](const std::string& line) {
    return starts_with(line, "deadline reached");
  });
  const auto ceiling = std::ranges::find_if(run.reporter.events, [](const std::string& line) {
    return starts_with(line, "temperature ceiling");
  });
  ASSERT_NE(deadline, run.reporter.events.end());
  ASSERT_NE(ceiling, run.reporter.events.end());
  EXPECT_LT(deadline, ceiling) << "the stop was already asked for when the machine got hot";
  EXPECT_EQ(result.value().workers[0].commands.sent, 1U) << "one Stop, not two";
  EXPECT_EQ(result.value().workers[1].commands.sent, 1U);
}

TEST(OrchestrationTemperatureTest, ANewWriteFailureOnTheStopIsTheControllersAndKillsTheWorkers) {
  Heated run(kPackage, then(repeated(2, {reads(60)}), repeated(1, {reads(95)})));
  run.syscalls.script_writes({{0, EIO}});
  run.syscalls.script_waits({{Reaped{kFirstPid, SIGKILL}, 0}, {Reaped{kSecondPid, SIGKILL}, 0}});
  auto result = run.go();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "control pipe");
  EXPECT_TRUE(run.syscalls.all_descriptors_closed());
}

// --- the journal ----------------------------------------------------------------------

TEST(OrchestrationTemperatureTest, TheJournalSaysWhichSensorWhatItReadAndTheCeiling) {
  Heated run(kPackage, then(repeated(2, {reads(60)}), repeated(1, {reads(92.5)})));
  controller_testing::RecordingWriter journal;
  run.options.journal = &journal;
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const auto entry = std::ranges::find_if(journal.entries, [](const journal::Entry& each) {
    return starts_with(each.phase, "ceiling");
  });
  ASSERT_NE(entry, journal.entries.end());
  EXPECT_EQ(entry->phase, "ceiling 85 C reached: thermal.package 0 (hwmon:mock)");
  EXPECT_EQ(entry->temperature_c, 92.5);
  EXPECT_EQ(entry->workers_alive, 2U);
  EXPECT_EQ(journal.entries.back().phase, "done") << "and the last word is still the end";
  EXPECT_LE(entry->phase.size(), journal::kMaxLabelBytes);
}

TEST(OrchestrationTemperatureTest, AnUnwatchedStopIsInTheJournalWithoutAReading) {
  Heated run(kPackage, then(repeated(2, {reads(60)}), repeated(1, {gone()})));
  run.options.temperature_blind_after_ns = 40 * kMs;
  controller_testing::RecordingWriter journal;
  run.options.journal = &journal;
  auto result = run.go();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const auto entry = std::ranges::find_if(journal.entries, [](const journal::Entry& each) {
    return starts_with(each.phase, "ceiling");
  });
  ASSERT_NE(entry, journal.entries.end());
  EXPECT_EQ(entry->phase, "ceiling unwatched 85 C");
  EXPECT_FALSE(entry->temperature_c.has_value());
}

// --- values ---------------------------------------------------------------------------

TEST(OrchestrationTemperatureTest, TheNewOptionsAndTheReportsStopTakePartInTheComparisons) {
  const RunOptions o;
  RunOptions p = o;
  EXPECT_EQ(o, p);
  const mocks::MockTimeline timeline;
  p.view = &timeline;
  EXPECT_NE(o, p);
  p = o;
  p.temperature_unread = "x";
  EXPECT_NE(o, p);
  p = o;
  p.temperature_blind_after_ns = 1;
  EXPECT_NE(o, p);
  EXPECT_EQ(o.temperature_blind_after_ns, 10'000'000'000);

  const RunReport r;
  RunReport s = r;
  EXPECT_EQ(r, s);
  s.temperature_stop = TemperatureStop{};
  EXPECT_NE(r, s);
}

}  // namespace
}  // namespace loadforge::controller
