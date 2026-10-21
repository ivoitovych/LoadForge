// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for the schedules: properties over thousands of option sets drawn from a
// generator of the test's own (std::mt19937_64, which the standard fixes, so the
// cases are the same everywhere). For the mixed workload: every schedule fits the
// run, its times are the rotations', its phases never repeat, its commands keep
// the rules of worker/control.hpp, sizes and seeds are what they are said to be.
// For the cycles: the commands applied to a model of each worker leave it at the
// level the step names, and no command is sent that changes nothing.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "controller/journal/journal.hpp"
#include "workloads/mixed/cycling.hpp"
#include "workloads/mixed/mixed.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

std::vector<RosterEntry> a_roster(std::mt19937_64& random) {
  const char* const names[] = {"adaptive_mesh", "compression", "dense_linear", "fft",
                               "graph",         "monte_carlo", "particles",    "raytrace",
                               "sort",          "sparse",      "stencil",      "unclassified"};
  std::vector<RosterEntry> roster;
  for (const char* name : names) {
    if (random() % 4 != 0) {
      roster.push_back(RosterEntry{name, (random() % 6 + 1) * (std::uint64_t{1} << 18U)});
    }
  }
  return roster;
}

TEST(MixedProperties, EveryScheduleFitsTheRunAndKeepsTheRulesOfItsCommands) {
  std::mt19937_64 random(20261021);
  int made = 0;
  for (int each = 0; each < 2000; ++each) {
    const std::vector<RosterEntry> roster = a_roster(random);
    MixedOptions options;
    options.workers = random() % 12 + 1;
    options.seed = random();
    options.rotation_ns = static_cast<std::int64_t>(random() % 40 + 1) * kSecond;
    options.duration_ns = static_cast<std::int64_t>(random() % 1500 + 1) * kSecond;
    options.footprint_bytes = (random() % 16 + 1) * (std::uint64_t{1} << 19U);
    options.duty = verification::DutyCycle{static_cast<std::uint32_t>(random() % 5),
                                           static_cast<std::uint32_t>(random() % 5)};
    const std::size_t sets = random() % 3 == 0 ? 0 : random() % 5 + 2;
    for (std::size_t set = 0; set < sets; ++set) {
      options.placements.push_back({static_cast<std::uint32_t>(set * 2)});
    }
    auto schedule = make_mixed(roster, options);
    const bool fits = std::ranges::any_of(roster, [&](const RosterEntry& entry) {
      return entry.min_footprint_bytes <= options.footprint_bytes;
    });
    if (!fits) {
      ASSERT_FALSE(schedule.has_value()) << each;
      continue;
    }
    ASSERT_TRUE(schedule.has_value()) << each << ": " << schedule.error();
    ++made;
    const Schedule& s = schedule.value();
    EXPECT_FALSE(problem(s, options.workers).has_value()) << each;
    EXPECT_EQ(s.start.size(), options.workers);
    EXPECT_EQ(s.steps.size(),
              static_cast<std::size_t>((options.duration_ns - 1) / options.rotation_ns));
    std::set<std::string> phases;
    std::set<std::uint64_t> seeds;
    for (std::size_t at = 0; at < s.steps.size(); ++at) {
      const Step& step = s.steps[at];
      EXPECT_EQ(step.at_ns, static_cast<std::int64_t>(at + 1) * options.rotation_ns);
      EXPECT_LT(step.at_ns, options.duration_ns);
      EXPECT_TRUE(phases.insert(step.phase).second) << "a phase repeated: " << step.phase;
      EXPECT_LE(step.phase.size(), journal::kMaxLabelBytes);
      EXPECT_LE(step.workload.size(), journal::kMaxLabelBytes);
      ASSERT_EQ(step.workers.size(), options.workers) << "every worker is told at every step";
      for (std::size_t slot = 0; slot < step.workers.size(); ++slot) {
        EXPECT_EQ(step.workers[slot].slot, slot);
        ASSERT_EQ(step.workers[slot].commands.size(), 1U);
        const worker::Command& command = step.workers[slot].commands[0];
        if (const auto* change = std::get_if<worker::Switch>(&command)) {
          const auto entry = std::ranges::find(roster, change->workload, &RosterEntry::name);
          ASSERT_NE(entry, roster.end()) << "a workload the roster does not have";
          EXPECT_GE(change->footprint_bytes, entry->min_footprint_bytes);
          EXPECT_TRUE(change->footprint_bytes == options.footprint_bytes ||
                      change->footprint_bytes == options.footprint_bytes / 2);
          EXPECT_EQ(change->duty, options.duty);
          EXPECT_TRUE(seeds.insert(change->seed).second) << "a seed twice";
        } else {
          const auto* move = std::get_if<worker::Rebind>(&command);
          ASSERT_NE(move, nullptr) << "a mixed schedule sends Switch and Rebind only";
          EXPECT_NE(std::ranges::find(options.placements, move->cpus), options.placements.end());
          EXPECT_EQ((at + 1) % 3, 0U) << "moves are every third rotation";
        }
      }
    }
    if (options.placements.empty()) {
      for (const Step& step : s.steps) {
        EXPECT_EQ(step.phase.find("move"), std::string::npos);
      }
    }
    for (const Start& start : s.start) {
      const auto entry = std::ranges::find(roster, start.workload, &RosterEntry::name);
      ASSERT_NE(entry, roster.end());
      EXPECT_GE(start.footprint_bytes, entry->min_footprint_bytes);
    }
    // And the same options are the same schedule.
    EXPECT_EQ(make_mixed(roster, options).value(), s);
  }
  EXPECT_GT(made, 1300);
}

TEST(MixedProperties, FiveHoursAtEveryRotationAFleetMightUseFitsAndIsValid) {
  const std::vector<RosterEntry> roster = {{"fft", 1U << 20U}, {"sort", 1U << 20U}};
  for (const std::int64_t rotation_s : {5, 30, 300, 1800}) {
    MixedOptions options;
    options.workers = 64;
    options.seed = 5;
    options.duration_ns = 5 * 3600 * kSecond;
    options.rotation_ns = rotation_s * kSecond;
    options.footprint_bytes = 4U << 20U;
    auto schedule = make_mixed(roster, options);
    ASSERT_TRUE(schedule.has_value()) << rotation_s << ": " << schedule.error();
    EXPECT_FALSE(problem(schedule.value(), 64).has_value());
    EXPECT_EQ(schedule.value().steps.size(), static_cast<std::size_t>((5 * 3600 - 1) / rotation_s));
  }
}

/// What a worker is doing, as the commands leave it.
struct Model {
  bool paused = false;
  std::uint16_t permille = 1000;
};

TEST(CyclingProperties, EveryLevelLeavesEveryWorkerAtTheLevelAndNoCommandChangesNothing) {
  std::mt19937_64 random(20261022);
  for (int each = 0; each < 3000; ++each) {
    CyclingOptions options;
    options.kind = random() % 2 == 0 ? CycleKind::kLoad : CycleKind::kCores;
    options.workers = random() % 40 + 1;
    options.dwell_ns = static_cast<std::int64_t>(random() % 20 + 1) * kSecond;
    options.duration_ns = static_cast<std::int64_t>(random() % 1000 + 1) * kSecond;
    options.period_ns = static_cast<std::int64_t>(random() % 5 + 1) * kSecond;
    auto made = make_cycling(options);
    ASSERT_TRUE(made.has_value()) << made.error();
    const Schedule& schedule = made.value();
    EXPECT_FALSE(problem(schedule, options.workers).has_value());
    EXPECT_EQ(schedule.steps.size(),
              static_cast<std::size_t>((options.duration_ns - 1) / options.dwell_ns) + 1);
    std::vector<Model> workers(options.workers);
    std::set<std::string> phases;
    for (std::size_t level = 0; level < schedule.steps.size(); ++level) {
      const Step& step = schedule.steps[level];
      EXPECT_EQ(step.at_ns, static_cast<std::int64_t>(level) * options.dwell_ns);
      EXPECT_TRUE(phases.insert(step.phase).second);
      std::set<std::size_t> named;
      for (const WorkerCommands& each_worker : step.workers) {
        EXPECT_TRUE(named.insert(each_worker.slot).second) << "a worker named twice in a step";
        EXPECT_FALSE(each_worker.commands.empty()) << "a worker named with nothing to do";
        Model& model = workers[each_worker.slot];
        for (const worker::Command& command : each_worker.commands) {
          const Model before = model;
          if (std::holds_alternative<worker::Pause>(command)) {
            EXPECT_FALSE(model.paused) << "a pause of a worker already paused";
            model.paused = true;
          } else if (std::holds_alternative<worker::Resume>(command)) {
            EXPECT_TRUE(model.paused) << "a resume of a worker not paused";
            model.paused = false;
          } else {
            const auto* duty = std::get_if<worker::SetDuty>(&command);
            ASSERT_NE(duty, nullptr) << "a cycle sends Pause, Resume and SetDuty only";
            EXPECT_NE(duty->busy_permille, model.permille) << "a duty already in force";
            EXPECT_EQ(duty->period_ns, options.period_ns);
            model.permille = duty->busy_permille;
          }
          EXPECT_TRUE(model.paused != before.paused || model.permille != before.permille);
        }
      }
      // The level, from the closed form of its name rather than from the steps before it.
      if (options.kind == CycleKind::kLoad) {
        const std::size_t at = level % 4;
        for (const Model& model : workers) {
          EXPECT_EQ(model.paused, at == 0) << step.phase;
          if (!model.paused) {
            EXPECT_EQ(model.permille, at == 2 ? 500 : 1000) << step.phase;
          }
        }
      } else {
        const std::uint16_t share[] = {125, 500, 1000, 250, 1000};
        const std::size_t working =
            std::max<std::size_t>(1, options.workers * share[level % 5] / 1000);
        for (std::size_t slot = 0; slot < workers.size(); ++slot) {
          EXPECT_EQ(workers[slot].paused, slot >= working) << step.phase << " worker " << slot;
        }
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::mixed
