// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the workload's forces against the direct O(n^2) summation on a subset of
// the particles, within the tolerance the specification derives.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "oracle/workloads/particles/direct_check.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

/// The worst residual over the tolerance, among the entries of `chosen`.
double worst_ratio(std::span<const std::byte> input, std::span<const std::byte> output,
                   std::span<const std::size_t> chosen) {
  const std::vector<double> positions = direct::doubles_of(input);
  const std::vector<double> forces = direct::doubles_of(output);
  double worst = 0.0;
  for (const std::size_t p : chosen) {
    const direct::Direct sum = direct::sum_directly(positions, p);
    for (std::size_t c = 0; c < 3; ++c) {
      const double residual = std::fabs(forces[3 * p + c] - sum.force[c]);
      worst = std::max(worst, residual / direct::tolerance_of(sum, c));
    }
  }
  return worst;
}

std::vector<std::size_t> every_th(std::size_t step) {
  std::vector<std::size_t> chosen;
  for (std::size_t p = 0; p < kParticles; p += step) {
    chosen.push_back(p);
  }
  chosen.push_back(kParticles - 1);
  return chosen;
}

TEST(ParticlesOracle, TheSampleIsSixteenDistinctParticlesIncludingTheFirstAndTheLast) {
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{77},
                                    ~std::uint64_t{0}}) {
    const std::vector<std::size_t> chosen = direct::sample(index);
    ASSERT_EQ(chosen.size(), kSampledParticles);
    EXPECT_TRUE(std::ranges::is_sorted(chosen));
    EXPECT_EQ(std::ranges::adjacent_find(chosen), chosen.end()) << "distinct";
    EXPECT_EQ(chosen.front(), 0U);
    EXPECT_EQ(chosen.back(), kParticles - 1);
    EXPECT_EQ(chosen, direct::sample(index)) << "a function of the index alone";
  }
  EXPECT_NE(direct::sample(1), direct::sample(2));
}

TEST(ParticlesOracle, TheForcesOfEveryDecompositionAndPathAgreeWithTheDirectSum) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{0xDEADBEEFCAFEF00D}}) {
        const auto workload = testing::make(testing::config_for(c, path, seed));
        ASSERT_NE(workload, nullptr);
        testing::PoolExecutor pool(3);
        for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{17}}) {
          const Bytes input = testing::generate(*workload, index);
          const Bytes output = testing::compute(*workload, index, input, pool);
          const direct::Result result = direct::check(index, input, output);
          EXPECT_EQ(result.verdict, direct::Verdict::kAgreement)
              << "c " << c << " " << simd::describe(path) << " seed " << seed << " unit " << index
              << " element " << result.element << " residual " << result.residual << " bound "
              << result.tolerance;
          EXPECT_EQ(result.entries_compared, kEntriesChecked);
        }
      }
    }
  }
}

TEST(ParticlesOracle, EveryThirtySecondParticleAgreesAndTheBoundIsNotLoose) {
  const std::vector<std::size_t> chosen = every_th(32);
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(c, path, 5));
      ASSERT_NE(workload, nullptr);
      testing::SerialExecutor serial;
      const Bytes input = testing::generate(*workload, 2);
      const Bytes output = testing::compute(*workload, 2, input, serial);
      const direct::Result result = direct::check_particles(input, output, chosen);
      EXPECT_EQ(result.verdict, direct::Verdict::kAgreement)
          << "c " << c << " " << simd::describe(path) << " element " << result.element;
      EXPECT_EQ(result.entries_compared, 3 * chosen.size());
      // The residual is rounding, a small fraction of a bound that covers the
      // worst case: a ratio of 1 would mean the bound is the error.
      EXPECT_LT(worst_ratio(input, output, chosen), 0.5);
    }
  }
}

Bytes clustered_input(double extent, std::uint64_t seed) {
  std::mt19937_64 engine(seed);
  std::vector<double> positions(3 * kParticles);
  for (double& value : positions) {
    value = std::uniform_real_distribution<double>(0.0, extent)(engine);
  }
  return testing::as_bytes(positions);
}

TEST(ParticlesOracle, ADenseClusterWithHundredsOfNeighboursAgreesWithinTheScaledBound) {
  const Bytes input = clustered_input(5.0, 99);
  const std::vector<std::size_t> chosen = every_th(256);
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(c, path));
      ASSERT_NE(workload, nullptr);
      testing::PoolExecutor pool(2);
      const Bytes output = testing::compute(*workload, 0, input, pool);
      const direct::Result result = direct::check_particles(input, output, chosen);
      EXPECT_EQ(result.verdict, direct::Verdict::kAgreement)
          << "c " << c << " " << simd::describe(path) << " element " << result.element
          << " residual " << result.residual << " bound " << result.tolerance;
      const std::vector<double> positions = direct::doubles_of(input);
      EXPECT_GT(direct::sum_directly(positions, 0).candidates, 300U) << "dense, as intended";
    }
  }
}

TEST(ParticlesOracle, ParticlesAtTheCornersOfTheBoxAgree) {
  // Half the particles in the corner at the origin, half in the corner at the
  // far end, including the extreme coordinates 0 and 16 - 2^-49.
  std::mt19937_64 engine(5);
  std::vector<double> positions(3 * kParticles);
  for (std::size_t p = 0; p < kParticles; ++p) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double offset = std::uniform_real_distribution<double>(0.0, 1.5)(engine);
      positions[3 * p + axis] = p % 2 == 0 ? offset : kBoxEdge - std::ldexp(1.0, -49) - offset;
    }
  }
  positions[0] = positions[1] = positions[2] = 0.0;
  positions[3] = positions[4] = positions[5] = kBoxEdge - std::ldexp(1.0, -49);
  const Bytes input = testing::as_bytes(positions);
  const std::vector<std::size_t> chosen = every_th(128);
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(c, path));
      ASSERT_NE(workload, nullptr);
      testing::SerialExecutor serial;
      const Bytes output = testing::compute(*workload, 0, input, serial);
      const direct::Result result = direct::check_particles(input, output, chosen);
      EXPECT_EQ(result.verdict, direct::Verdict::kAgreement)
          << "c " << c << " " << simd::describe(path) << " element " << result.element;
    }
  }
}

TEST(ParticlesOracle, ParticlesAtOnePointExertNothingOnOneAnother) {
  const Bytes input = testing::as_bytes(std::vector<double>(3 * kParticles, 7.5));
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(c, path));
      ASSERT_NE(workload, nullptr);
      testing::SerialExecutor serial;
      const Bytes output = testing::compute(*workload, 0, input, serial);
      EXPECT_EQ(direct::check(0, input, output).verdict, direct::Verdict::kAgreement);
      for (const double force : testing::as_doubles(output)) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(force), 0U) << "exactly +0";
      }
    }
  }
}

TEST(ParticlesOracle, ParticlesAtDistanceExactlyTheCutoffExertNothing) {
  // A lattice of spacing two: every neighbour is at distance 2 (q = 4, t = 0)
  // or at the same point, and the sum is exactly zero whichever cells are
  // searched.
  std::vector<double> positions(3 * kParticles);
  for (std::size_t p = 0; p < kParticles; ++p) {
    positions[3 * p] = 2.0 * static_cast<double>(p % 8);
    positions[3 * p + 1] = 2.0 * static_cast<double>(p / 8 % 8);
    positions[3 * p + 2] = 2.0 * static_cast<double>(p / 64 % 8);
  }
  const Bytes input = testing::as_bytes(positions);
  for (const std::uint32_t c : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(c, path));
      ASSERT_NE(workload, nullptr);
      testing::SerialExecutor serial;
      const Bytes output = testing::compute(*workload, 0, input, serial);
      for (const double force : testing::as_doubles(output)) {
        ASSERT_EQ(std::bit_cast<std::uint64_t>(force), 0U);
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
