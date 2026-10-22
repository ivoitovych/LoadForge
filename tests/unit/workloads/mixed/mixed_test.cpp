// SPDX-License-Identifier: GPL-3.0-or-later
//
// The flagship's schedule: what it refuses, the phases it moves through, how
// workers are assigned, and the sizes and seeds it chooses.

#include "workloads/mixed/mixed.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "controller/journal/journal.hpp"
#include "worker/control.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

/// All eleven workloads, as the build has them, each accepting a megabyte.
std::vector<RosterEntry> every_workload() {
  std::vector<RosterEntry> roster;
  for (const char* name : {"adaptive_mesh", "compression", "dense_linear", "fft", "graph",
                           "monte_carlo", "particles", "raytrace", "sort", "sparse", "stencil"}) {
    roster.push_back(RosterEntry{name, kMiB});
  }
  return roster;
}

MixedOptions options_for(std::size_t workers) {
  MixedOptions options;
  options.workers = workers;
  options.seed = 7;
  options.duration_ns = 70 * kSecond;
  options.rotation_ns = 10 * kSecond;
  options.footprint_bytes = 8 * kMiB;
  return options;
}

Schedule schedule_of(const std::vector<RosterEntry>& roster, const MixedOptions& options) {
  auto made = make_mixed(roster, options);
  EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error());
  return made.has_value() ? made.value() : Schedule{};
}

const worker::Switch& switch_of(const Step& step, std::size_t slot) {
  const auto* change = std::get_if<worker::Switch>(&step.workers[slot].commands[0]);
  EXPECT_NE(change, nullptr) << step.phase << " worker " << slot;
  static const worker::Switch kNone;
  return change != nullptr ? *change : kNone;
}

TEST(MixedClasses, EveryWorkloadHasAClassAndAnUnknownOneIsBalanced) {
  for (const char* name : {"dense_linear", "fft", "monte_carlo", "particles", "raytrace"}) {
    EXPECT_EQ(classify(name), Intensity::kCompute) << name;
  }
  for (const char* name : {"adaptive_mesh", "graph", "sort", "sparse", "stencil"}) {
    EXPECT_EQ(classify(name), Intensity::kMemory) << name;
  }
  EXPECT_EQ(classify("compression"), Intensity::kBalanced);
  EXPECT_EQ(classify("a_workload_added_later"), Intensity::kBalanced);
  EXPECT_EQ(classify(""), Intensity::kBalanced);
}

TEST(MixedRefusals, EveryOptionThatMakesNoScheduleIsRefusedInWords) {
  const std::vector<RosterEntry> roster = every_workload();
  const auto refusal = [&roster](const MixedOptions& options) {
    auto made = make_mixed(roster, options);
    EXPECT_FALSE(made.has_value());
    return made.has_value() ? std::string{} : made.error();
  };
  MixedOptions options = options_for(4);
  options.workers = 0;
  EXPECT_EQ(refusal(options), "a mixed schedule needs at least one worker");
  options = options_for(4);
  options.duration_ns = 0;
  EXPECT_EQ(refusal(options), "the duration and the rotation interval must be positive");
  options.duration_ns = -1;
  EXPECT_EQ(refusal(options), "the duration and the rotation interval must be positive");
  options = options_for(4);
  options.rotation_ns = 0;
  EXPECT_EQ(refusal(options), "the duration and the rotation interval must be positive");
  options = options_for(4);
  options.footprint_bytes = 0;
  EXPECT_EQ(refusal(options), "a mixed schedule needs a footprint");
  options = options_for(4);
  options.rotation_ns = kSecond / 1000;  // A millisecond over two hundred seconds.
  options.duration_ns = 200 * kSecond;
  EXPECT_EQ(refusal(options), "a rotation every 1 ms for 200000 ms is more than 100000 steps");
  options = options_for(4);
  options.placements = {{0, 1}};
  EXPECT_EQ(refusal(options), "workers move across at least two sets of CPUs, not one");
  for (const std::vector<std::uint32_t>& bad :
       {std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{3, 2},
        std::vector<std::uint32_t>{1024}}) {
    options = options_for(4);
    options.placements = {{0}, bad};
    EXPECT_EQ(refusal(options),
              "a set of CPUs to move across is empty, unordered or past CPU 1023");
  }
  options = options_for(4);
  options.footprint_bytes = kMiB / 2;  // Nothing in the roster accepts so little.
  EXPECT_EQ(refusal(options), "no workload of the roster accepts a footprint of 524288 bytes");
  EXPECT_EQ(make_mixed({}, options_for(4)).error(),
            "no workload of the roster accepts a footprint of 8388608 bytes");
}

TEST(MixedRefusals, TheScheduleHoldsNoMoreCommandsThanMemoryIsWillingToHold) {
  // One command per worker per step: a thousand workers over a hundred thousand steps is a
  // hundred million commands, which no schedule needs and no run should allocate.
  MixedOptions options = options_for(1000);
  options.rotation_ns = 1 * kSecond;
  options.duration_ns = 100'000 * kSecond;
  auto made = make_mixed(every_workload(), options);
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error(), "1000 worker(s) over 99999 steps is more than 4000000 commands");
}

TEST(MixedSchedule, ARunOfSeventySecondsAtTenSecondsARotationHasSixStepsAfterTheStart) {
  const Schedule schedule = schedule_of(every_workload(), options_for(4));
  EXPECT_EQ(schedule.workers, 4U);
  ASSERT_EQ(schedule.start.size(), 4U);
  ASSERT_EQ(schedule.steps.size(), 6U) << "the rotations at 10 s ... 60 s; 70 s is the end";
  for (std::size_t at = 0; at < schedule.steps.size(); ++at) {
    EXPECT_EQ(schedule.steps[at].at_ns, static_cast<std::int64_t>(at + 1) * 10 * kSecond);
    EXPECT_EQ(schedule.steps[at].workers.size(), 4U) << "every worker is told at every step";
  }
  EXPECT_FALSE(problem(schedule, 4).has_value());
}

TEST(MixedSchedule, ARotationAtTheEndOfTheRunIsNotMade) {
  MixedOptions options = options_for(2);
  options.duration_ns = 20 * kSecond;
  EXPECT_EQ(schedule_of(every_workload(), options).steps.size(), 1U) << "10 s, not 20 s";
  options.duration_ns = 20 * kSecond + 1;
  EXPECT_EQ(schedule_of(every_workload(), options).steps.size(), 2U);
  options.duration_ns = 5 * kSecond;
  EXPECT_TRUE(schedule_of(every_workload(), options).steps.empty()) << "shorter than a rotation";
}

TEST(MixedSchedule, ThePhasesFollowTheirOrderAndNameWhatTheMachineIsDoing) {
  MixedOptions options = options_for(4);
  options.duration_ns = 110 * kSecond;
  const Schedule schedule = schedule_of(every_workload(), options);
  EXPECT_EQ(schedule.phase, "r0 mixed");
  std::vector<std::string> phases;
  for (const Step& step : schedule.steps) {
    phases.push_back(step.phase);
  }
  EXPECT_EQ(phases,
            (std::vector<std::string>{
                "r1 compute-heavy", "r2 memory-heavy", "r3 balanced", "r4 all-join", "r5 mixed",
                "r6 compute-heavy", "r7 memory-heavy", "r8 balanced", "r9 all-join", "r10 mixed"}));
  EXPECT_EQ(std::set<std::string>(phases.begin(), phases.end()).size(), phases.size())
      << "a phase label never repeats, so that a counter's advance can be told per phase";
}

TEST(MixedSchedule, EachPhaseAssignsWorkloadsOfItsKind) {
  MixedOptions options = options_for(4);
  options.duration_ns = 60 * kSecond;
  const Schedule schedule = schedule_of(every_workload(), options);
  ASSERT_EQ(schedule.steps.size(), 5U);
  const auto kind_of = [&](std::size_t step, std::size_t slot) {
    return classify(switch_of(schedule.steps[step], slot).workload);
  };
  for (std::size_t slot = 0; slot < 4; ++slot) {
    EXPECT_EQ(kind_of(0, slot), Intensity::kCompute) << "compute-heavy, worker " << slot;
    EXPECT_EQ(kind_of(1, slot), Intensity::kMemory) << "memory-heavy, worker " << slot;
  }
  // Balanced: the even workers compute and the odd workers wait on memory.
  for (std::size_t slot = 0; slot < 4; ++slot) {
    EXPECT_EQ(kind_of(2, slot), slot % 2 == 0 ? Intensity::kCompute : Intensity::kMemory);
  }
  // All join: one workload for everyone.
  const std::string joined = switch_of(schedule.steps[3], 0).workload;
  for (std::size_t slot = 1; slot < 4; ++slot) {
    EXPECT_EQ(switch_of(schedule.steps[3], slot).workload, joined);
  }
  EXPECT_EQ(schedule.steps[3].workload, joined);
  // Mixed: neighbours differ, as far as the roster goes.
  std::set<std::string> distinct;
  for (std::size_t slot = 0; slot < 4; ++slot) {
    distinct.insert(switch_of(schedule.steps[4], slot).workload);
  }
  EXPECT_EQ(distinct.size(), 4U);
}

TEST(MixedSchedule, APhaseWhoseKindTheRosterLacksUsesTheWholeRoster) {
  const std::vector<RosterEntry> only_memory = {{"sort", kMiB}, {"graph", kMiB}};
  MixedOptions options = options_for(2);
  options.duration_ns = 40 * kSecond;
  const Schedule schedule = schedule_of(only_memory, options);
  ASSERT_EQ(schedule.steps.size(), 3U);
  for (const Step& step : schedule.steps) {
    for (std::size_t slot = 0; slot < 2; ++slot) {
      EXPECT_TRUE(switch_of(step, slot).workload == "sort" ||
                  switch_of(step, slot).workload == "graph")
          << step.phase;
    }
  }
  const std::vector<RosterEntry> one = {{"fft", kMiB}};
  const Schedule alone = schedule_of(one, options);
  for (const Step& step : alone.steps) {
    EXPECT_EQ(step.workload, "fft");
  }
}

TEST(MixedSchedule, TheStartIsTheFirstRotationMadeAsAnyOtherAndTheLabelsSayWhatItIs) {
  const Schedule schedule = schedule_of(every_workload(), options_for(3));
  ASSERT_EQ(schedule.start.size(), 3U);
  std::set<std::string> names;
  for (const Start& start : schedule.start) {
    names.insert(start.workload);
    EXPECT_TRUE(start.footprint_bytes == 8 * kMiB || start.footprint_bytes == 4 * kMiB);
  }
  EXPECT_EQ(names.size(), 3U) << "neighbours differ";
  std::string label;
  for (const std::string& name : names) {
    label += label.empty() ? "" : "+";
    label += name;
  }
  EXPECT_EQ(schedule.workload, label);
}

TEST(MixedSchedule, DatasetsChangeSizeByHalfAndNeverBelowWhatTheWorkloadAccepts) {
  MixedOptions options = options_for(8);
  options.duration_ns = 600 * kSecond;
  options.footprint_bytes = 8 * kMiB;
  std::vector<RosterEntry> roster = every_workload();
  roster[0].min_footprint_bytes = 6 * kMiB;  // adaptive_mesh cannot go to half.
  const Schedule schedule = schedule_of(roster, options);
  std::set<std::uint64_t> sizes;
  for (const Step& step : schedule.steps) {
    for (std::size_t slot = 0; slot < step.workers.size(); ++slot) {
      const worker::Switch& change = switch_of(step, slot);
      sizes.insert(change.footprint_bytes);
      EXPECT_TRUE(change.footprint_bytes == 8 * kMiB || change.footprint_bytes == 4 * kMiB);
      if (change.workload == "adaptive_mesh") {
        EXPECT_EQ(change.footprint_bytes, 8 * kMiB);
      }
      EXPECT_EQ(change.decomposition, 0U) << "the workload's own default";
    }
  }
  EXPECT_EQ(sizes, (std::set<std::uint64_t>{4 * kMiB, 8 * kMiB})) << "both are used";
}

TEST(MixedSchedule, EveryRotationOfEveryWorkerHasItsOwnSeedAndTheDutyCycleIsTheRuns) {
  MixedOptions options = options_for(4);
  options.duration_ns = 300 * kSecond;
  options.duty = verification::DutyCycle{3, 7};
  const Schedule schedule = schedule_of(every_workload(), options);
  std::set<std::uint64_t> seeds;
  std::size_t total = 0;
  for (const Step& step : schedule.steps) {
    for (std::size_t slot = 0; slot < step.workers.size(); ++slot) {
      seeds.insert(switch_of(step, slot).seed);
      ++total;
      EXPECT_EQ(switch_of(step, slot).duty, (verification::DutyCycle{3, 7}));
    }
  }
  EXPECT_EQ(seeds.size(), total);
}

TEST(MixedSchedule, WorkersMoveAcrossTheirPlacementsOnEveryThirdRotationAndSwitchNothingThen) {
  MixedOptions options = options_for(3);
  options.duration_ns = 70 * kSecond;
  options.placements = {{0}, {1}, {2}};
  const Schedule schedule = schedule_of(every_workload(), options);
  ASSERT_EQ(schedule.steps.size(), 6U);
  for (std::size_t at = 0; at < schedule.steps.size(); ++at) {
    const Step& step = schedule.steps[at];
    const bool moves = (at + 1) % 3 == 0;
    EXPECT_EQ(step.phase.substr(step.phase.find(' ') + 1) == "move", moves) << step.phase;
    for (std::size_t slot = 0; slot < 3; ++slot) {
      ASSERT_EQ(step.workers[slot].commands.size(), 1U);
      EXPECT_EQ(std::holds_alternative<worker::Rebind>(step.workers[slot].commands[0]), moves);
    }
  }
  // Rotation 3: worker s takes placement (s + 1) mod 3, a permutation: no CPU holds two workers.
  const Step& first_move = schedule.steps[2];
  EXPECT_EQ(std::get<worker::Rebind>(first_move.workers[0].commands[0]).cpus,
            (std::vector<std::uint32_t>{1}));
  EXPECT_EQ(std::get<worker::Rebind>(first_move.workers[1].commands[0]).cpus,
            (std::vector<std::uint32_t>{2}));
  EXPECT_EQ(std::get<worker::Rebind>(first_move.workers[2].commands[0]).cpus,
            (std::vector<std::uint32_t>{0}));
  const Step& second_move = schedule.steps[5];
  EXPECT_EQ(std::get<worker::Rebind>(second_move.workers[0].commands[0]).cpus,
            (std::vector<std::uint32_t>{2}));
  // A move changes where, not what: the workload label stays what the machine was doing.
  EXPECT_EQ(first_move.workload, schedule.steps[1].workload);
  EXPECT_FALSE(problem(schedule, 3).has_value());
}

TEST(MixedSchedule, WithoutPlacementsNoWorkerIsMoved) {
  MixedOptions options = options_for(3);
  options.duration_ns = 300 * kSecond;
  const Schedule schedule = schedule_of(every_workload(), options);
  for (const Step& step : schedule.steps) {
    for (const WorkerCommands& each : step.workers) {
      EXPECT_FALSE(std::holds_alternative<worker::Rebind>(each.commands[0])) << step.phase;
    }
  }
}

TEST(MixedSchedule, ALongRunsLabelsFitTheJournal) {
  MixedOptions options = options_for(16);
  options.duration_ns = 3600 * kSecond;
  options.rotation_ns = 30 * kSecond;
  const Schedule schedule = schedule_of(every_workload(), options);
  EXPECT_EQ(schedule.steps.size(), 119U);
  for (const Step& step : schedule.steps) {
    EXPECT_LE(step.phase.size(), journal::kMaxLabelBytes);
    EXPECT_LE(step.workload.size(), journal::kMaxLabelBytes);
  }
  EXPECT_LE(schedule.workload.size(), journal::kMaxLabelBytes);
}

TEST(MixedSchedule, TheSameOptionsAreTheSameScheduleAndAnotherSeedIsAnother) {
  const MixedOptions options = options_for(4);
  EXPECT_EQ(schedule_of(every_workload(), options), schedule_of(every_workload(), options));
  MixedOptions other = options;
  other.seed = 8;
  EXPECT_NE(digest(schedule_of(every_workload(), options)),
            digest(schedule_of(every_workload(), other)));
}

TEST(MixedSchedule, ALongPeriodDoesNotOverflowTheTimes) {
  MixedOptions options = options_for(1);
  options.duration_ns = INT64_MAX;
  options.rotation_ns = INT64_MAX / 4;
  const Schedule schedule = schedule_of(every_workload(), options);
  ASSERT_EQ(schedule.steps.size(), 4U);
  EXPECT_EQ(schedule.steps.back().at_ns, (INT64_MAX / 4) * 4);
}

}  // namespace
}  // namespace loadforge::mixed
