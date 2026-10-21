// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the forces on generated and on corrupted units -- the
// balance of the forces (Newton's third law), the symmetries of the box, the
// independence of everything but the input, and totality over corrupt input.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "oracle/workloads/particles/direct_check.hpp"
#include "property/workloads/particles/lattice.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

struct Variant {
  std::uint32_t cells;
  simd::IsaPath path;
};

std::vector<Variant> variants() {
  std::vector<Variant> all;
  for (const std::uint32_t cells : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      all.push_back({cells, path});
    }
  }
  return all;
}

std::string name_of(const Variant& variant) {
  return "c" + std::to_string(variant.cells) + " " + std::string(simd::describe(variant.path));
}

TEST(ParticlesProperty, TheForcesOnAllParticlesSumToZeroWithinTheBoundOfTheSummation) {
  // Each term is computed as the exact negative of its partner (d, q, t and w
  // are the same numbers with d negated), so the computed terms cancel in
  // exact arithmetic and the sum of the n computed forces is the rounding of
  // adding them: at most gamma_M S per particle and component, M the number
  // of nonzero terms and S the sum of their absolute values.
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path, 31));
    ASSERT_NE(workload, nullptr);
    testing::PoolExecutor pool(4);
    const Bytes input = testing::generate(*workload, 6);
    const std::vector<double> forces =
        testing::as_doubles(testing::compute(*workload, 6, input, pool));
    const std::vector<double> positions = testing::as_doubles(input);
    std::array<double, 3> total{};
    std::array<double, 3> bound{};
    for (std::size_t p = 0; p < kParticles; ++p) {
      const direct::Direct sum = direct::sum_directly(positions, p);
      std::size_t active = 0;
      for (std::size_t j = 0; j < kParticles; ++j) {
        const double dx = positions[3 * p] - positions[3 * j];
        const double dy = positions[3 * p + 1] - positions[3 * j + 1];
        const double dz = positions[3 * p + 2] - positions[3 * j + 2];
        if (j != p && 1.0 - (dx * dx + dy * dy + dz * dz) / 4.0 > 0.0) {
          ++active;
        }
      }
      for (std::size_t c = 0; c < 3; ++c) {
        total[c] += forces[3 * p + c];
        bound[c] += gamma(active) * sum.abs_sum[c];
      }
    }
    for (std::size_t c = 0; c < 3; ++c) {
      // The total is itself a sum of 4096 numbers: its own rounding adds
      // gamma_4096 times the sum of |F|.
      double magnitude = 0.0;
      for (std::size_t p = 0; p < kParticles; ++p) {
        magnitude += std::fabs(forces[3 * p + c]);
      }
      EXPECT_LE(std::fabs(total[c]), 1.01 * bound[c] + 1.01 * gamma(kParticles) * magnitude)
          << name_of(variant) << " component " << c;
      EXPECT_GT(magnitude, 100.0) << "the forces are not trivially zero";
    }
  }
}

TEST(ParticlesProperty, RelabellingTheParticlesRelabelsTheForcesWithinTheTolerance) {
  std::mt19937_64 engine(77);
  std::vector<std::size_t> permutation(kParticles);
  std::iota(permutation.begin(), permutation.end(), std::size_t{0});
  std::shuffle(permutation.begin(), permutation.end(), engine);
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path, 9));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    const Bytes input = testing::generate(*workload, 3);
    const std::vector<double> positions = testing::as_doubles(input);
    std::vector<double> shuffled(positions.size());
    for (std::size_t p = 0; p < kParticles; ++p) {
      for (std::size_t c = 0; c < 3; ++c) {
        shuffled[3 * permutation[p] + c] = positions[3 * p + c];
      }
    }
    const std::vector<double> original =
        testing::as_doubles(testing::compute(*workload, 3, input, serial));
    const std::vector<double> relabelled =
        testing::as_doubles(testing::compute(*workload, 3, testing::as_bytes(shuffled), serial));
    bool any_bit_differs = false;
    for (const std::size_t p : direct::sample(3)) {
      const direct::Direct sum = direct::sum_directly(positions, p);
      for (std::size_t c = 0; c < 3; ++c) {
        const double a = original[3 * p + c];
        const double b = relabelled[3 * permutation[p] + c];
        EXPECT_LE(std::fabs(a - b), 2.0 * direct::tolerance_of(sum, c))
            << name_of(variant) << " particle " << p << " component " << c;
        any_bit_differs = any_bit_differs || bits(a) != bits(b);
      }
    }
    // The summation order did change (so this is a test of the tolerance, not
    // of equality): relabelling moves particles between positions in cells.
    EXPECT_TRUE(any_bit_differs) << name_of(variant);
  }
}

TEST(ParticlesProperty, MirroringTheBoxInXFlipsTheXForceAndLeavesTheOthersWithinTheTolerance) {
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path, 12));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    const Bytes input = testing::generate(*workload, 1);
    const std::vector<double> positions = testing::as_doubles(input);
    std::vector<double> mirrored = positions;
    for (std::size_t p = 0; p < kParticles; ++p) {
      ASSERT_GT(positions[3 * p], 0.0) << "0 would map to the open end of the box";
      mirrored[3 * p] = kBoxEdge - positions[3 * p];  // exact: multiples of 2^-49 below 16
    }
    const std::vector<double> original =
        testing::as_doubles(testing::compute(*workload, 1, input, serial));
    const std::vector<double> flipped =
        testing::as_doubles(testing::compute(*workload, 1, testing::as_bytes(mirrored), serial));
    for (const std::size_t p : direct::sample(1)) {
      const direct::Direct sum = direct::sum_directly(positions, p);
      for (std::size_t c = 0; c < 3; ++c) {
        const double expected = c == 0 ? -original[3 * p] : original[3 * p + c];
        EXPECT_LE(std::fabs(flipped[3 * p + c] - expected), 2.0 * direct::tolerance_of(sum, c))
            << name_of(variant) << " particle " << p << " component " << c;
      }
    }
  }
}

TEST(ParticlesProperty, TheOutputDependsOnTheInputAndNotOnTheUnitIndex) {
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    const Bytes input = testing::generate(*workload, 4);
    EXPECT_EQ(testing::compute(*workload, 4, input, serial),
              testing::compute(*workload, 999, input, serial))
        << name_of(variant);
  }
}

TEST(ParticlesProperty, AParticleMovedBeyondTheCutoffOfTheOthersChangesNothingForThem) {
  // A cluster of four, and a fifth that is first inside its reach and then
  // far outside it: the scalar path is bit-identical (a term that is exactly
  // zero adds nothing); the lanes' accumulation tree may shift with the zero
  // candidate, so the vector path agrees within the tolerance of the sum.
  const std::vector<testing::Point> cluster = {
      {9.0, 9.0, 9.0}, {9.5, 9.25, 9.0}, {9.25, 9.75, 9.5}, {10.0, 9.5, 9.25}};
  for (const Variant& variant : variants()) {
    std::vector<testing::Point> before = cluster;
    before.push_back({9.5, 9.5, 9.5});
    std::vector<testing::Point> after = cluster;
    after.push_back({9.5, 9.5, 9.5 + 3.0});
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    const auto forces = [&](const std::vector<testing::Point>& test) {
      return testing::as_doubles(
          testing::compute(*workload, 0, testing::as_bytes(testing::isolated_unit(test)), serial));
    };
    // The cluster alone, with the fifth particle parked at once: exactly as
    // `after`, whose fifth particle is out of reach of the four.
    const std::vector<double> with = forces(before);
    const std::vector<double> without = forces(after);
    EXPECT_NE(with[0], without[0]) << "inside its reach the fifth particle matters";
    std::vector<testing::Point> four = cluster;
    four.push_back({9.5, 9.5, 12.5});
    EXPECT_EQ(forces(four), without) << name_of(variant) << ": the fifth, anywhere beyond reach";
  }
}

TEST(ParticlesProperty, TheForceOnAParticleDoesNotDependOnWhereTheOthersAreOutsideItsCutoff) {
  // The same particle, 0, with the same neighbours, and in the second unit
  // every other particle moved to a different place at least 2 from it.
  const std::vector<testing::Point> near = {
      {5.0, 5.0, 5.0}, {5.5, 5.25, 5.0}, {4.5, 5.0, 5.75}, {5.0, 4.25, 4.5}};
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    const std::vector<double> alone = testing::as_doubles(
        testing::compute(*workload, 0, testing::as_bytes(testing::isolated_unit(near)), serial));
    std::vector<double> scattered = testing::isolated_unit(near);
    std::mt19937_64 engine(5);
    for (std::size_t p = near.size(); p < kParticles; ++p) {
      for (std::size_t c = 0; c < 3; ++c) {
        scattered[3 * p + c] = 11.0 + std::uniform_real_distribution<double>(0.0, 5.0)(engine);
      }
    }
    // Particles near (13.5, 13.5, 13.5) are in the box's far corner, 8+ away
    // from the four: they repel one another but not the four.
    const std::vector<double> moved =
        testing::as_doubles(testing::compute(*workload, 0, testing::as_bytes(scattered), serial));
    for (std::size_t k = 0; k < 3 * near.size(); ++k) {
      if (variant.path == simd::IsaPath::kScalar) {
        EXPECT_EQ(bits(moved[k]), bits(alone[k])) << name_of(variant) << " entry " << k;
      } else {
        EXPECT_NEAR(moved[k], alone[k], 1e-14) << name_of(variant) << " entry " << k;
      }
    }
  }
}

TEST(ParticlesProperty, ComputeIsTotalAndDeterministicOverBitFlipsAnywhereInTheInput) {
  std::mt19937_64 engine(2024);
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path, 3));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    for (int trial = 0; trial < 12; ++trial) {
      Bytes input = testing::generate(*workload, static_cast<std::uint64_t>(trial));
      const int flips = 1 + trial % 5;
      for (int flip = 0; flip < flips; ++flip) {
        const std::size_t at = engine() % input.size();
        input[at] ^= static_cast<std::byte>(1U << (engine() % 8));
      }
      const Bytes first = testing::compute(*workload, 0, input, serial, std::byte{0x00});
      const Bytes second = testing::compute(*workload, 0, input, serial, std::byte{0xFF});
      ASSERT_EQ(first.size(), kOutputBytes);
      EXPECT_EQ(first, second) << name_of(variant) << " trial " << trial;
    }
  }
}

TEST(ParticlesProperty, EveryDoubleThatIsNotAFiniteCoordinateInTheBoxIsComputedOn) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const std::vector<double> specials = {nan,
                                        -nan,
                                        inf,
                                        -inf,
                                        -0.0,
                                        0.0,
                                        -1.0,
                                        16.0,
                                        1e300,
                                        -1e300,
                                        std::numeric_limits<double>::denorm_min(),
                                        std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::lowest(),
                                        15.999999999999998};
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path, 3));
    ASSERT_NE(workload, nullptr);
    testing::SerialExecutor serial;
    for (const double special : specials) {
      std::vector<double> positions = testing::as_doubles(testing::generate(*workload, 0));
      for (std::size_t k = 0; k < positions.size(); k += 211) {
        positions[k] = special;
      }
      const Bytes input = testing::as_bytes(positions);
      const Bytes once = testing::compute(*workload, 0, input, serial, std::byte{0x11});
      EXPECT_EQ(testing::compute(*workload, 0, input, serial, std::byte{0xEE}), once)
          << name_of(variant) << " special " << special;
    }
  }
}

TEST(ParticlesProperty, AnUnitOfParticlesAllInOneCellStillHasAboundedAmountOfWork) {
  // Every particle at one point of one cell: n^2 terms at most, all zero. The
  // run must finish and answer exactly zero -- it is the worst case for the
  // neighbour search, and the same cell holds everything for any corrupt
  // input that collapses the positions.
  for (const Variant& variant : variants()) {
    const auto workload = testing::make(testing::config_for(variant.cells, variant.path));
    ASSERT_NE(workload, nullptr);
    testing::PoolExecutor pool(3);
    const Bytes input = testing::as_bytes(std::vector<double>(3 * kParticles, 3.5));
    const std::vector<double> forces =
        testing::as_doubles(testing::compute(*workload, 0, input, pool));
    EXPECT_TRUE(std::ranges::all_of(forces, [](double f) { return bits(f) == 0; }))
        << name_of(variant);
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
