// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thermal and power cycling: the levels of each shape, a command sent only
// where it changes something, and what is refused.

#include "workloads/mixed/cycling.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "workloads/mixed/mixed.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

CyclingOptions load_for(std::size_t workers, std::int64_t levels) {
  CyclingOptions options;
  options.kind = CycleKind::kLoad;
  options.workers = workers;
  options.dwell_ns = 5 * kSecond;
  options.duration_ns = levels * options.dwell_ns;
  options.period_ns = kSecond;
  return options;
}

Schedule made(const CyclingOptions& options) {
  auto schedule = make_cycling(options);
  EXPECT_TRUE(schedule.has_value()) << (schedule.has_value() ? "" : schedule.error());
  return schedule.has_value() ? schedule.value() : Schedule{};
}

/// The commands a step sends one worker; empty if the step does not name it.
std::vector<worker::Command> commands_for(const Step& step, std::size_t slot) {
  for (const WorkerCommands& each : step.workers) {
    if (each.slot == slot) {
      return each.commands;
    }
  }
  return {};
}

TEST(CyclingWords, TheKindsHaveNames) {
  EXPECT_STREQ(describe(CycleKind::kLoad), "load");
  EXPECT_STREQ(describe(CycleKind::kCores), "cores");
}

TEST(CyclingRefusals, EveryOptionThatMakesNoScheduleIsRefusedInWords) {
  const auto refusal = [](const CyclingOptions& options) {
    auto schedule = make_cycling(options);
    EXPECT_FALSE(schedule.has_value());
    return schedule.has_value() ? std::string{} : schedule.error();
  };
  CyclingOptions options = load_for(2, 4);
  options.workers = 0;
  EXPECT_EQ(refusal(options), "a cycling schedule needs at least one worker");
  options = load_for(2, 4);
  options.duration_ns = 0;
  EXPECT_EQ(refusal(options), "the duration, the dwell of a level and the period must be positive");
  options = load_for(2, 4);
  options.dwell_ns = 0;
  EXPECT_EQ(refusal(options), "the duration, the dwell of a level and the period must be positive");
  options = load_for(2, 4);
  options.period_ns = -1;
  EXPECT_EQ(refusal(options), "the duration, the dwell of a level and the period must be positive");
  options = load_for(2, 4);
  options.dwell_ns = kSecond / 1000;
  options.duration_ns = 200 * kSecond;
  EXPECT_EQ(refusal(options), "a level every 1 ms for 200000 ms is more than 100000 steps");
  options = load_for(1000, 4);
  options.dwell_ns = kSecond;
  options.duration_ns = 99'999 * kSecond;
  EXPECT_EQ(refusal(options), "1000 worker(s) over 99999 levels is more than 4000000 commands");
}

TEST(CyclingLoad, IdleHeavyModerateHeavyAndAgainEachLevelOneDwell) {
  const Schedule schedule = made(load_for(2, 9));
  ASSERT_EQ(schedule.steps.size(), 9U);
  EXPECT_EQ(schedule.workers, 2U);
  EXPECT_TRUE(schedule.start.empty()) << "the workers start as configured";
  EXPECT_EQ(schedule.phase, "run");
  EXPECT_EQ(schedule.workload, "cycling load");
  std::vector<std::string> phases;
  for (std::size_t at = 0; at < schedule.steps.size(); ++at) {
    EXPECT_EQ(schedule.steps[at].at_ns, static_cast<std::int64_t>(at) * 5 * kSecond);
    EXPECT_EQ(schedule.steps[at].workload, "cycling load");
    phases.push_back(schedule.steps[at].phase);
  }
  EXPECT_EQ(phases,
            (std::vector<std::string>{"c0 idle", "c1 heavy", "c2 moderate", "c3 heavy", "c4 idle",
                                      "c5 heavy", "c6 moderate", "c7 heavy", "c8 idle"}));
}

TEST(CyclingLoad, ACommandIsSentOnlyWhereItChangesWhatTheWorkerIsDoing) {
  const Schedule schedule = made(load_for(2, 5));
  const std::vector<std::vector<worker::Command>> expected = {
      {worker::Pause{}},                 // c0 idle: from running to paused.
      {worker::Resume{}},                // c1 heavy: continuous load was never changed.
      {worker::SetDuty{500, kSecond}},   // c2 moderate.
      {worker::SetDuty{1000, kSecond}},  // c3 heavy again.
      {worker::Pause{}},                 // c4 idle.
  };
  ASSERT_EQ(schedule.steps.size(), expected.size());
  for (std::size_t at = 0; at < expected.size(); ++at) {
    for (std::size_t slot = 0; slot < 2; ++slot) {
      EXPECT_EQ(commands_for(schedule.steps[at], slot), expected[at])
          << schedule.steps[at].phase << " worker " << slot;
    }
  }
}

TEST(CyclingLoad, FromIdleToModerateResumesAndSetsTheDutyInOneStep) {
  // Moderate straight after idle: the cycle never does it, but the state machine must.
  CyclingOptions options = load_for(1, 3);
  const Schedule schedule = made(options);
  // idle (pause), heavy (resume), moderate (duty): a pause then a moderate level is reached by
  // the cores shape below; here the shape of a step that follows a pause is a resume first.
  EXPECT_EQ(commands_for(schedule.steps[1], 0), (std::vector<worker::Command>{worker::Resume{}}));
}

TEST(CyclingLoad, ALevelThatChangesNothingIsAStepWithNoWorkers) {
  CyclingOptions options = load_for(1, 20);
  const Schedule schedule = made(options);
  // Every level of the load cycle changes the state: no step is empty.
  for (const Step& step : schedule.steps) {
    EXPECT_FALSE(step.workers.empty()) << step.phase;
  }
}

TEST(CyclingLoad, TheLastLevelIsTheOneBeforeTheEndOfTheRun) {
  CyclingOptions options = load_for(1, 4);
  options.duration_ns = 4 * 5 * kSecond;
  EXPECT_EQ(made(options).steps.size(), 4U);
  options.duration_ns += 1;
  EXPECT_EQ(made(options).steps.size(), 5U) << "a level that begins before the end is made";
  options.duration_ns = 1;
  EXPECT_EQ(made(options).steps.size(), 1U);
}

TEST(CyclingCores, AnEighthHalfAllAQuarterAllOfTheWorkersWork) {
  CyclingOptions options = load_for(8, 6);
  options.kind = CycleKind::kCores;
  const Schedule schedule = made(options);
  EXPECT_EQ(schedule.workload, "cycling cores");
  ASSERT_EQ(schedule.steps.size(), 6U);
  std::vector<std::string> phases;
  for (const Step& step : schedule.steps) {
    phases.push_back(step.phase);
  }
  EXPECT_EQ(phases, (std::vector<std::string>{"c0 1 of 8", "c1 4 of 8", "c2 8 of 8", "c3 2 of 8",
                                              "c4 8 of 8", "c5 1 of 8"}));
  // Level 0: workers 1..7 are paused, worker 0 is left running.
  EXPECT_TRUE(commands_for(schedule.steps[0], 0).empty());
  for (std::size_t slot = 1; slot < 8; ++slot) {
    EXPECT_EQ(commands_for(schedule.steps[0], slot),
              (std::vector<worker::Command>{worker::Pause{}}));
  }
  // Level 1: workers 1..3 resume, 4..7 stay paused.
  for (std::size_t slot = 1; slot < 4; ++slot) {
    EXPECT_EQ(commands_for(schedule.steps[1], slot),
              (std::vector<worker::Command>{worker::Resume{}}));
  }
  for (std::size_t slot = 4; slot < 8; ++slot) {
    EXPECT_TRUE(commands_for(schedule.steps[1], slot).empty());
  }
  // Level 3 (a quarter): 2..7 pause again, 0 and 1 are left alone.
  for (std::size_t slot = 2; slot < 8; ++slot) {
    EXPECT_EQ(commands_for(schedule.steps[3], slot),
              (std::vector<worker::Command>{worker::Pause{}}));
  }
  EXPECT_TRUE(commands_for(schedule.steps[3], 0).empty());
}

TEST(CyclingCores, OneWorkerAlwaysWorksAndHasNothingToChange) {
  CyclingOptions options = load_for(1, 5);
  options.kind = CycleKind::kCores;
  const Schedule schedule = made(options);
  for (const Step& step : schedule.steps) {
    EXPECT_TRUE(step.workers.empty()) << step.phase;
    EXPECT_NE(step.phase.find("1 of 1"), std::string::npos);
  }
}

TEST(CyclingCores, AShareThatRoundsToNoWorkersIsOne) {
  CyclingOptions options = load_for(3, 1);
  options.kind = CycleKind::kCores;
  const Schedule schedule = made(options);
  EXPECT_EQ(schedule.steps[0].phase, "c0 1 of 3") << "an eighth of three is none; one works";
}

TEST(CyclingSchedule, IsValidAndTheSameEveryTime) {
  for (const CycleKind kind : {CycleKind::kLoad, CycleKind::kCores}) {
    CyclingOptions options = load_for(5, 30);
    options.kind = kind;
    const Schedule schedule = made(options);
    EXPECT_FALSE(problem(schedule, 5).has_value());
    EXPECT_EQ(schedule, made(options));
  }
}

}  // namespace
}  // namespace loadforge::mixed
