// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the schedules: the C++ against a second implementation of them in
// Python (tests/fixtures/workloads/mixed/generate.py), written from the
// headers' account and sharing nothing with the C++. The two are compared as
// text, line for line, and the digest at the end of each listing is the
// Python's own CRC-32 of its own canonical text.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "oracle/workloads/mixed/listing.hpp"
#include "workloads/mixed/cycling.hpp"
#include "workloads/mixed/mixed.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

std::string fixture(const std::string& name) {
  std::ifstream in(std::string{LOADFORGE_SOURCE_DIR} + "/tests/fixtures/workloads/mixed/" + name);
  EXPECT_TRUE(in.good()) << name;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<RosterEntry> every_workload() {
  std::vector<RosterEntry> roster;
  for (const char* name : {"adaptive_mesh", "compression", "dense_linear", "fft", "graph",
                           "monte_carlo", "particles", "raytrace", "sort", "sparse", "stencil"}) {
    roster.push_back(RosterEntry{name, std::uint64_t{1} << 20U});
  }
  return roster;
}

/// The first line on which two listings differ, with both, or empty.
std::string first_difference(const std::string& expected, const std::string& actual) {
  std::size_t line = 1;
  std::size_t from = 0;
  std::size_t other = 0;
  while (from < expected.size() || other < actual.size()) {
    const std::size_t end = expected.find('\n', from);
    const std::size_t end_other = actual.find('\n', other);
    const std::string a = expected.substr(from, end == std::string::npos ? end : end - from);
    const std::string b =
        actual.substr(other, end_other == std::string::npos ? end_other : end_other - other);
    if (a != b) {
      return "line " + std::to_string(line) + ": the Python says \"" + a + "\", the C++ \"" + b +
             "\"";
    }
    from = end == std::string::npos ? expected.size() : end + 1;
    other = end_other == std::string::npos ? actual.size() : end_other + 1;
    ++line;
  }
  return {};
}

Schedule mixed_of(std::size_t workers, std::uint64_t seed, std::int64_t duration_s,
                  std::int64_t rotation_s, std::uint64_t footprint,
                  std::vector<std::vector<std::uint32_t>> placements) {
  MixedOptions options;
  options.workers = workers;
  options.seed = seed;
  options.duration_ns = duration_s * kSecond;
  options.rotation_ns = rotation_s * kSecond;
  options.footprint_bytes = footprint;
  options.placements = std::move(placements);
  auto made = make_mixed(every_workload(), options);
  EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error());
  return made.has_value() ? made.value() : Schedule{};
}

Schedule cycling_of(CycleKind kind, std::size_t workers, std::int64_t duration_s,
                    std::int64_t dwell_s) {
  CyclingOptions options;
  options.kind = kind;
  options.workers = workers;
  options.duration_ns = duration_s * kSecond;
  options.dwell_ns = dwell_s * kSecond;
  auto made = make_cycling(options);
  EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error());
  return made.has_value() ? made.value() : Schedule{};
}

TEST(ScheduleOracle, TheMixedWorkloadWithTheWorkersMovingAcrossFourCpus) {
  const std::string expected = fixture("mixed-w4-s7-d70-r10-moving.txt");
  const std::string actual =
      mixed_testing::listing(mixed_of(4, 7, 70, 10, 8388608, {{0}, {1}, {2}, {3}}));
  EXPECT_EQ(first_difference(expected, actual), "");
  EXPECT_EQ(expected, actual);
}

TEST(ScheduleOracle, TheMixedWorkloadWithNoPlacements) {
  const std::string expected = fixture("mixed-w3-s1-d300-r10-fixed.txt");
  const std::string actual = mixed_testing::listing(mixed_of(3, 1, 300, 10, 8388608, {}));
  EXPECT_EQ(first_difference(expected, actual), "");
  EXPECT_EQ(expected, actual);
}

TEST(ScheduleOracle, ThirtyMinutesOfEightWorkersOnSharedCpuSets) {
  const std::string expected = fixture("mixed-w8-s42-d1800-r30-moving.txt");
  const std::string actual =
      mixed_testing::listing(mixed_of(8, 42, 1800, 30, 16777216, {{0, 1}, {2, 3}, {4, 5}, {6, 7}}));
  EXPECT_EQ(first_difference(expected, actual), "");
  EXPECT_EQ(expected, actual);
}

TEST(ScheduleOracle, TheLoadCycle) {
  const std::string expected = fixture("cycling-load-w3-d100-dwell5.txt");
  const std::string actual = mixed_testing::listing(cycling_of(CycleKind::kLoad, 3, 100, 5));
  EXPECT_EQ(first_difference(expected, actual), "");
  EXPECT_EQ(expected, actual);
}

TEST(ScheduleOracle, TheCoreCycleOfEightWorkersAndOfOne) {
  for (const auto& [name, workers, duration] :
       {std::tuple{"cycling-cores-w8-d60-dwell5.txt", std::size_t{8}, std::int64_t{60}},
        std::tuple{"cycling-cores-w1-d20-dwell5.txt", std::size_t{1}, std::int64_t{20}}}) {
    const std::string expected = fixture(name);
    const std::string actual =
        mixed_testing::listing(cycling_of(CycleKind::kCores, workers, duration, 5));
    EXPECT_EQ(first_difference(expected, actual), "") << name;
    EXPECT_EQ(expected, actual) << name;
  }
}

}  // namespace
}  // namespace loadforge::mixed
