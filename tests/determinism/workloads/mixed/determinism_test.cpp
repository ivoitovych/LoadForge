// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for the schedules: a schedule is a pure function of its options, so it is
// the same on every call, on every thread, in any order, and -- the claim that
// matters for a tool run on x86-64 and on ARM64 -- on both architectures. The
// last is pinned by digests that come from the Python implementation
// (tests/fixtures/workloads/mixed/generate.py), not from this code, and CI
// runs this test on both.

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "workloads/mixed/cycling.hpp"
#include "workloads/mixed/mixed.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

std::vector<RosterEntry> every_workload() {
  std::vector<RosterEntry> roster;
  for (const char* name : {"adaptive_mesh", "compression", "dense_linear", "fft", "graph",
                           "monte_carlo", "particles", "raytrace", "sort", "sparse", "stencil"}) {
    roster.push_back(RosterEntry{name, std::uint64_t{1} << 20U});
  }
  return roster;
}

MixedOptions the_moving_case() {
  MixedOptions options;
  options.workers = 4;
  options.seed = 7;
  options.duration_ns = 70 * kSecond;
  options.rotation_ns = 10 * kSecond;
  options.footprint_bytes = 8388608;
  options.placements = {{0}, {1}, {2}, {3}};
  return options;
}

// The last line of each fixture: "digest XXXXXXXX".
constexpr std::uint32_t kMovingDigest = 0x530E584A;         // mixed-w4-s7-d70-r10-moving.txt
constexpr std::uint32_t kFixedDigest = 0xC47AB111;          // mixed-w3-s1-d300-r10-fixed.txt
constexpr std::uint32_t kThirtyMinutesDigest = 0x03EB5F7F;  // mixed-w8-s42-d1800-r30-moving.txt
constexpr std::uint32_t kLoadDigest = 0x1701CEB2;           // cycling-load-w3-d100-dwell5.txt
constexpr std::uint32_t kCoresDigest = 0x1FECDC26;          // cycling-cores-w8-d60-dwell5.txt
constexpr std::uint32_t kOneCoreDigest = 0x9A57B5A4;        // cycling-cores-w1-d20-dwell5.txt

Schedule made(const MixedOptions& options) {
  auto schedule = make_mixed(every_workload(), options);
  EXPECT_TRUE(schedule.has_value());
  return schedule.has_value() ? schedule.value() : Schedule{};
}

Schedule cycled(CycleKind kind, std::size_t workers, std::int64_t duration_s,
                std::int64_t dwell_s) {
  CyclingOptions options;
  options.kind = kind;
  options.workers = workers;
  options.duration_ns = duration_s * kSecond;
  options.dwell_ns = dwell_s * kSecond;
  auto schedule = make_cycling(options);
  EXPECT_TRUE(schedule.has_value());
  return schedule.has_value() ? schedule.value() : Schedule{};
}

TEST(ScheduleDeterminism, TheDigestsAreThePythonsOnEveryArchitecture) {
  EXPECT_EQ(digest(made(the_moving_case())), kMovingDigest);
  MixedOptions fixed;
  fixed.workers = 3;
  fixed.seed = 1;
  fixed.duration_ns = 300 * kSecond;
  fixed.rotation_ns = 10 * kSecond;
  fixed.footprint_bytes = 8388608;
  EXPECT_EQ(digest(made(fixed)), kFixedDigest);
  MixedOptions long_run;
  long_run.workers = 8;
  long_run.seed = 42;
  long_run.duration_ns = 1800 * kSecond;
  long_run.rotation_ns = 30 * kSecond;
  long_run.footprint_bytes = 16777216;
  long_run.placements = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
  EXPECT_EQ(digest(made(long_run)), kThirtyMinutesDigest);
  EXPECT_EQ(digest(cycled(CycleKind::kLoad, 3, 100, 5)), kLoadDigest);
  EXPECT_EQ(digest(cycled(CycleKind::kCores, 8, 60, 5)), kCoresDigest);
  EXPECT_EQ(digest(cycled(CycleKind::kCores, 1, 20, 5)), kOneCoreDigest);
}

TEST(ScheduleDeterminism, TheSameOptionsGiveTheSameScheduleEveryTimeAndOnEveryThread) {
  const Schedule reference = made(the_moving_case());
  for (int again = 0; again < 20; ++again) {
    EXPECT_EQ(made(the_moving_case()), reference);
  }
  std::vector<Schedule> from_threads(4);
  std::vector<std::thread> threads;
  for (std::size_t each = 0; each < from_threads.size(); ++each) {
    threads.emplace_back([&from_threads, each] { from_threads[each] = made(the_moving_case()); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const Schedule& each : from_threads) {
    EXPECT_EQ(each, reference);
  }
}

TEST(ScheduleDeterminism, WhatWasMadeBeforeDoesNotChangeWhatIsMadeNext) {
  const Schedule alone = made(the_moving_case());
  MixedOptions other = the_moving_case();
  other.seed = 99;
  (void)made(other);
  (void)cycled(CycleKind::kLoad, 5, 50, 5);
  EXPECT_EQ(made(the_moving_case()), alone) << "a schedule keeps no state between calls";
}

TEST(ScheduleDeterminism, ADifferentSeedIsADifferentRunAndTheSameSeedIsTheSameRun) {
  MixedOptions a = the_moving_case();
  MixedOptions b = a;
  b.seed = a.seed + 1;
  EXPECT_NE(digest(made(a)), digest(made(b)));
  EXPECT_EQ(digest(made(a)), digest(made(a)));
}

}  // namespace
}  // namespace loadforge::mixed
