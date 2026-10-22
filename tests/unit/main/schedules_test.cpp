// SPDX-License-Identifier: GPL-3.0-or-later
//
// The runs `stress --test mixed` and `stress --test cycling` are made of: the
// defaults, the workers' configuration from the schedule, and what is refused.

#include "main/schedules.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "controller/plan.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

TEST(ScheduleDefaults, ARotationIsASixthOfTheRunBetweenASecondAndFiveMinutes) {
  EXPECT_EQ(default_rotation_ms(10'000), 1'666);
  EXPECT_EQ(default_rotation_ms(1'000), 1'000) << "not under a second";
  EXPECT_EQ(default_rotation_ms(0), 1'000);
  EXPECT_EQ(default_rotation_ms(5 * 3'600'000), 300'000) << "five hours rotate every five minutes";
  EXPECT_EQ(default_rotation_ms(30 * 60'000), 300'000);
  EXPECT_EQ(default_rotation_ms(30 * 60'000 - 1), 299'999);
}

TEST(ScheduleDefaults, ALevelIsAnEighthOfTheRunBetweenASecondAndAMinute) {
  EXPECT_EQ(default_dwell_ms(16'000), 2'000);
  EXPECT_EQ(default_dwell_ms(1'000), 1'000);
  EXPECT_EQ(default_dwell_ms(8 * 3'600'000), 60'000);
  EXPECT_EQ(default_dwell_ms(8 * 60'000), 60'000);
  EXPECT_EQ(default_dwell_ms(8 * 60'000 - 8), 59'999);
}

MixedRequest a_mixed_request(const workloads::Registry& registry) {
  MixedRequest request;
  request.registry = &registry;
  request.workers = 4;
  request.cpus = {0, 1, 2, 3};
  request.seed = 3;
  request.duration_ms = 60'000;
  request.footprint_bytes = 8 * kMiB;
  return request;
}

TEST(PlanMixed, AWorkerPerCpuStartsAsTheSchedulesStartAndMovesAcrossTheCpus) {
  const workloads::BuiltinRegistry registry;
  const MixedRequest request = a_mixed_request(registry);
  auto planned = plan_mixed(request);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  const ScheduledRun& run = planned.value();
  EXPECT_EQ(run.config.run.mode, config::Mode::kStress);
  EXPECT_EQ(run.config.run.duration.count(), 60'000);
  ASSERT_EQ(run.config.workloads.size(), 4U);
  ASSERT_EQ(run.schedule.start.size(), 4U);
  for (std::size_t slot = 0; slot < 4; ++slot) {
    const config::Workload& each = run.config.workloads[slot];
    EXPECT_EQ(each.name, run.schedule.start[slot].workload);
    EXPECT_TRUE(each.enabled);
    EXPECT_EQ(each.threads, config::Threads::count(1));
    ASSERT_TRUE(each.memory.has_value());
    EXPECT_EQ(each.memory->byte_count(), run.schedule.start[slot].footprint_bytes);
  }
  EXPECT_EQ(run.schedule.steps.size(), 5U) << "the run is 60 s and a rotation 10 s";
  bool moves = false;
  for (const mixed::Step& step : run.schedule.steps) {
    moves = moves || step.phase.find("move") != std::string::npos;
  }
  EXPECT_TRUE(moves) << "four CPUs, four workers: they rotate across them";
}

TEST(PlanMixed, OneCpuOrOneWorkerMovesNothing) {
  const workloads::BuiltinRegistry registry;
  for (const auto& [workers, cpus] :
       {std::pair<std::size_t, std::vector<std::uint32_t>>{4, {0}},
        std::pair<std::size_t, std::vector<std::uint32_t>>{1, {0, 1}}}) {
    MixedRequest request = a_mixed_request(registry);
    request.workers = workers;
    request.cpus = cpus;
    request.duration_ms = 300'000;
    auto planned = plan_mixed(request);
    ASSERT_TRUE(planned.has_value()) << planned.error();
    for (const mixed::Step& step : planned.value().schedule.steps) {
      EXPECT_EQ(step.phase.find("move"), std::string::npos) << step.phase;
    }
  }
}

TEST(PlanMixed, TheRotationAskedForIsTheRotationMade) {
  const workloads::BuiltinRegistry registry;
  MixedRequest request = a_mixed_request(registry);
  request.rotation_ms = 20'000;
  auto planned = plan_mixed(request);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  EXPECT_EQ(planned.value().schedule.steps.size(), 2U);
  EXPECT_EQ(planned.value().schedule.steps[0].at_ns, 20'000'000'000);
}

TEST(PlanMixed, WhatIsRefusedIsRefusedInTheSchedulesWords) {
  const workloads::BuiltinRegistry registry;
  MixedRequest request = a_mixed_request(registry);
  request.duration_ms = 0;
  EXPECT_EQ(plan_mixed(request).error(), "the duration is not one a schedule can be made for");
  request.duration_ms = INT64_MAX / 1000;
  EXPECT_EQ(plan_mixed(request).error(), "the duration is not one a schedule can be made for");
  request = a_mixed_request(registry);
  request.workers = 0;
  EXPECT_EQ(plan_mixed(request).error(), "a mixed schedule needs at least one worker");
  request = a_mixed_request(registry);
  request.footprint_bytes = 1024;
  EXPECT_EQ(plan_mixed(request).error(),
            "no workload of the roster accepts a footprint of 1024 bytes");
}

TEST(PlanCycling, EveryWorkerRunsTheWorkloadAndTheScheduleCyclesIt) {
  CyclingRequest request;
  request.workload = "fft";
  request.kind = mixed::CycleKind::kCores;
  request.workers = 4;
  request.duration_ms = 40'000;
  request.footprint = core::ByteSize::bytes(4 * kMiB);
  auto planned = plan_cycling(request);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  const ScheduledRun& run = planned.value();
  ASSERT_EQ(run.config.workloads.size(), 4U);
  for (const config::Workload& each : run.config.workloads) {
    EXPECT_EQ(each.name, "fft");
    EXPECT_EQ(each.memory, request.footprint);
  }
  EXPECT_TRUE(run.schedule.start.empty());
  EXPECT_EQ(run.schedule.steps.size(), 8U) << "40 s in dwells of 5 s";
  EXPECT_EQ(run.schedule.workload, "cycling cores");
}

TEST(PlanCycling, ADwellAndAFootprintLeftOutAreTheDefaultsAndRefusalsAreSaid) {
  CyclingRequest request;
  request.workload = "fft";
  request.workers = 2;
  request.duration_ms = 16'000;
  auto planned = plan_cycling(request);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  EXPECT_EQ(planned.value().schedule.steps.size(), 8U) << "a level every 2 s";
  EXPECT_FALSE(planned.value().config.workloads[0].memory.has_value());
  request.dwell_ms = 4'000;
  EXPECT_EQ(plan_cycling(request).value().schedule.steps.size(), 4U);
  request.duration_ms = 0;
  EXPECT_EQ(plan_cycling(request).error(), "the duration is not one a schedule can be made for");
  request.duration_ms = INT64_MAX / 1000;
  EXPECT_EQ(plan_cycling(request).error(), "the duration is not one a schedule can be made for");
  request.duration_ms = 16'000;
  request.workers = 0;
  EXPECT_EQ(plan_cycling(request).error(), "a cycling schedule needs at least one worker");
}

}  // namespace
}  // namespace loadforge::cli
