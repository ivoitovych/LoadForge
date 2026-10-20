// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for the adaptive mesh workload, class BoundedResidual (determinism.md
// §2). What is promised, and so what is tested:
//
//   * Exact, on every schedule: the output's leaf counts, and the output of a
//     schedule that is itself reproducible (one slot: Serial; and a dealt
//     schedule, whose grouping is a function of its seed) from run to run.
//   * Bounded: the integrals, on every schedule and thread count, within
//     cell_tolerance() of the reference sum -- including the claiming
//     executor, where which thread values which leaf is the scheduler's
//     choice, run repeatedly on real threads.
//   * The class is not vacuous: two dealt schedules of one unit give integrals
//     that differ in bits, in some cell, by no more than twice the tolerance
//     (each is within the tolerance of the reference). A BoundedResidual
//     workload whose results never differed would be a BitExact one wrongly
//     labelled loose; here the difference is demonstrated.
//   * A committed digest of the counts and of the one-slot output, so the same
//     bits come from x86-64 and ARM64 (the leaf values are plain binary64
//     operations in a fixed order, spec.hpp): the fixture
//     tests/fixtures/golden/workloads/adaptive_mesh/output-crc32.txt.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "core/crc32.hpp"
#include "unit/workloads/adaptive_mesh/support.hpp"

namespace loadforge::workloads::adaptive_mesh::testing {
namespace {

Bytes counts_of(const Bytes& output) {
  return {output.begin() + static_cast<std::ptrdiff_t>(kCountsOffset), output.end()};
}

TEST(AdaptiveMeshDeterminism, CountsAreExactAndIntegralsBoundedOnEverySchedule) {
  const auto workload = make(config_for(31, 8));
  ASSERT_NE(workload, nullptr);
  Serial serial;
  for (std::uint64_t unit = 0; unit < 3; ++unit) {
    const Bytes input = generated(*workload, unit);
    const Hotspots spots = hotspots_of(input);
    const Bytes base = computed(*workload, unit, input, serial);
    for (const std::size_t threads : {1U, 2U, 3U, 4U, 7U}) {
      for (const std::uint64_t seed : {1U, 2U}) {
        Dealt dealt(threads, seed);
        Shuffled shuffled(threads, seed);
        for (worker::Executor* executor :
             {static_cast<worker::Executor*>(&dealt), static_cast<worker::Executor*>(&shuffled)}) {
          const Bytes output = computed(*workload, unit, input, *executor);
          EXPECT_EQ(counts_of(output), counts_of(base))
              << "unit " << unit << ", " << threads << " threads, seed " << seed;
          EXPECT_EQ(first_disagreement(output, spots), "");
        }
      }
    }
  }
}

TEST(AdaptiveMeshDeterminism, ARepeatableScheduleRepeatsItsBits) {
  const auto workload = make(config_for(31, 8));
  ASSERT_NE(workload, nullptr);
  const Bytes input = generated(*workload, 1);
  Serial serial;
  EXPECT_EQ(computed(*workload, 1, input, serial), computed(*workload, 1, input, serial));
  Dealt first(5, 9);
  Dealt second(5, 9);
  EXPECT_EQ(computed(*workload, 1, input, first), computed(*workload, 1, input, second));
  Shuffled threaded_first(5, 9);
  Shuffled threaded_second(5, 9);
  EXPECT_EQ(computed(*workload, 1, input, threaded_first),
            computed(*workload, 1, input, threaded_second))
      << "real threads, but each slot's leaves and their order are a function of the seed";
}

TEST(AdaptiveMeshDeterminism, TheClaimingScheduleStaysBoundedRunAfterRun) {
  const auto workload = make(config_for(31, 8));
  ASSERT_NE(workload, nullptr);
  const Bytes input = generated(*workload, 2);
  const Hotspots spots = hotspots_of(input);
  Serial serial;
  const Bytes base = computed(*workload, 2, input, serial);
  for (const std::size_t threads : {2U, 4U, 7U}) {
    Claiming claiming(threads);
    for (int run = 0; run < 12; ++run) {
      const Bytes output = computed(*workload, 2, input, claiming);
      ASSERT_EQ(counts_of(output), counts_of(base));
      ASSERT_EQ(first_disagreement(output, spots), "") << threads << " threads, run " << run;
    }
  }
}

TEST(AdaptiveMeshDeterminism, TwoGroupingsOfTheLeavesDifferInBitsWithinTwiceTheTolerance) {
  const auto workload = make(config_for(31, 8));
  ASSERT_NE(workload, nullptr);
  const Bytes input = generated(*workload, 0);
  const Hotspots spots = hotspots_of(input);
  Dealt one(3, 1);
  Dealt two(7, 2);
  const Bytes a = computed(*workload, 0, input, one);
  const Bytes b = computed(*workload, 0, input, two);
  std::size_t differing = 0;
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    const double x = integral_of(a, cell);
    const double y = integral_of(b, cell);
    if (std::bit_cast<std::uint64_t>(x) != std::bit_cast<std::uint64_t>(y)) {
      ++differing;
      const CellReference r = reference_cell(cell, spots);
      EXPECT_LE(std::fabs(x - y), 2.0 * cell_tolerance(r.leaves, r.value));
    }
  }
  EXPECT_GT(differing, 0U) << "a BoundedResidual workload shows its residual";
}

// ---- committed digests ------------------------------------------------------------

/// The lines this machine computes, in the fixture's format.
std::map<std::string, std::string> computed_digests() {
  std::map<std::string, std::string> lines;
  const auto workload = make(config_for(1, 2));
  Serial serial;
  for (const std::uint64_t unit : {std::uint64_t{0}, std::uint64_t{1}}) {
    const Bytes input = generated(*workload, unit);
    const Bytes output = computed(*workload, unit, input, serial);
    const auto hex = [](std::uint32_t value) {
      std::ostringstream text;
      text << std::hex << std::setw(8) << std::setfill('0') << value;
      return text.str();
    };
    lines["input " + std::to_string(unit)] = hex(core::crc32(input));
    lines["counts " + std::to_string(unit)] = hex(core::crc32(counts_of(output)));
    lines["serial " + std::to_string(unit)] = hex(core::crc32(output));
  }
  return lines;
}

TEST(AdaptiveMeshDeterminism, DigestsMatchTheCommittedFixtureOnThisArchitecture) {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/golden/workloads/adaptive_mesh/output-crc32.txt");
  ASSERT_TRUE(file.good());
  std::map<std::string, std::string> committed;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t split = line.rfind(' ');
    ASSERT_NE(split, std::string::npos) << line;
    committed[line.substr(0, split)] = line.substr(split + 1);
  }
  const auto computed_lines = computed_digests();
  // Printed whole, so that a new machine shows all of its lines at once.
  for (const auto& [key, value] : computed_lines) {
    std::cout << key << ' ' << value << '\n';
  }
  EXPECT_EQ(computed_lines, committed);
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh::testing
