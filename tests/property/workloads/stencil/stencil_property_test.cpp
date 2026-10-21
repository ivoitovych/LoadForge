// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the workload that hold for any input of its domain, tried
// on grids drawn as the specification draws them (and on the edges of the
// domain), on every path this machine runs. Each is a fact about a Jacobi
// sweep of non-negative weights summing to one, stated so that breaking the
// kernel breaks it.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/stencil/stencil_support.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;

/// Every (path, tile) pair this machine runs, two tile edges per path.
struct Setting {
  simd::IsaPath path;
  std::uint32_t tile;
};

std::vector<Setting> settings() {
  std::vector<Setting> all;
  for (const simd::IsaPath path : testing::runnable_paths()) {
    all.push_back({path, 16});
    all.push_back({path, 128});
  }
  return all;
}

Bytes run(const Setting& setting, const Bytes& input) {
  const auto workload = testing::make(testing::config_for(setting.path, setting.tile));
  testing::SerialExecutor serial;
  return testing::computed(*workload, 0, input, serial);
}

std::string name_of(const Setting& setting) {
  return std::string(simd::describe(setting.path)) + "/" + std::to_string(setting.tile);
}

bool same_bits(double lhs, double rhs) {
  return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

Bytes transposed(const Bytes& grid) {
  Bytes result(kGridBytes);
  for (std::size_t row = 0; row < kEdge; ++row) {
    for (std::size_t column = 0; column < kEdge; ++column) {
      // NOLINTNEXTLINE(readability-suspicious-call-argument): the transpose
      testing::set_cell(result, column, row, testing::cell_of(grid, row, column));
    }
  }
  return result;
}

Bytes flipped_rows(const Bytes& grid) {
  Bytes result(kGridBytes);
  for (std::size_t row = 0; row < kEdge; ++row) {
    for (std::size_t column = 0; column < kEdge; ++column) {
      testing::set_cell(result, kEdge - 1 - row, column, testing::cell_of(grid, row, column));
    }
  }
  return result;
}

double largest_difference(const Bytes& lhs, const Bytes& rhs) {
  double largest = 0.0;
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    largest = std::max(largest, std::fabs(testing::cell_of(lhs, cell / kEdge, cell % kEdge) -
                                          testing::cell_of(rhs, cell / kEdge, cell % kEdge)));
  }
  return largest;
}

TEST(StencilProperty, NoCellEverExceedsTheLargestCellOfTheInput) {
  for (const Setting& setting : settings()) {
    for (const int exponent : {0, -3, -40}) {
      const Bytes input = testing::random_grid(11, 1, exponent);
      const Bytes output = run(setting, input);
      const double m0 = testing::max_magnitude(input);
      // The maximum principle, up to the rounding the bound allows.
      EXPECT_LE(testing::max_magnitude(output), m0 * (1.0 + 2 * gamma(4 * kSteps)))
          << name_of(setting) << " exponent " << exponent;
    }
  }
}

TEST(StencilProperty, ARandomRingIsCopiedBitForBit) {
  for (const Setting& setting : settings()) {
    for (const std::uint64_t stream : {2U, 3U}) {
      Bytes input = testing::random_grid(21, stream);
      // Ring cells with arbitrary bit patterns, not only generated values.
      Bytes noise(kGridBytes);
      rng::fill_stream(77, stream, noise);
      for (std::size_t at = 0; at < kEdge; ++at) {
        for (const auto& [row, column] : {std::pair{std::size_t{0}, at},
                                          {kEdge - 1, at},
                                          {at, std::size_t{0}},
                                          {at, kEdge - 1}}) {
          std::memcpy(input.data() + (row * kEdge + column) * 8,
                      noise.data() + (row * kEdge + column) * 8, 8);
        }
      }
      const Bytes output = run(setting, input);
      for (std::size_t at = 0; at < kEdge; ++at) {
        for (const auto& [row, column] : {std::pair{std::size_t{0}, at},
                                          {kEdge - 1, at},
                                          {at, std::size_t{0}},
                                          {at, kEdge - 1}}) {
          ASSERT_EQ(std::memcmp(input.data() + (row * kEdge + column) * 8,
                                output.data() + (row * kEdge + column) * 8, 8),
                    0)
              << name_of(setting) << " " << row << "," << column;
        }
      }
    }
  }
}

TEST(StencilProperty, NegatingTheInputNegatesTheOutputBitForBit) {
  for (const Setting& setting : settings()) {
    const Bytes input = testing::random_grid(31, 4);
    Bytes negated(kGridBytes);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      testing::set_cell(negated, cell / kEdge, cell % kEdge,
                        -testing::cell_of(input, cell / kEdge, cell % kEdge));
    }
    const Bytes output = run(setting, input);
    const Bytes opposite = run(setting, negated);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      ASSERT_TRUE(same_bits(-testing::cell_of(output, cell / kEdge, cell % kEdge),
                            testing::cell_of(opposite, cell / kEdge, cell % kEdge)))
          << name_of(setting) << " cell " << cell;
    }
  }
}

TEST(StencilProperty, ScalingByAPowerOfTwoScalesTheOutputBitForBit) {
  for (const Setting& setting : settings()) {
    const Bytes input = testing::random_grid(41, 5);
    const Bytes output = run(setting, input);
    for (const int exponent : {-30, -1, 1, 30}) {
      Bytes scaled(kGridBytes);
      for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
        testing::set_cell(
            scaled, cell / kEdge, cell % kEdge,
            std::ldexp(testing::cell_of(input, cell / kEdge, cell % kEdge), exponent));
      }
      const Bytes result = run(setting, scaled);
      for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
        ASSERT_TRUE(
            same_bits(std::ldexp(testing::cell_of(output, cell / kEdge, cell % kEdge), exponent),
                      testing::cell_of(result, cell / kEdge, cell % kEdge)))
            << name_of(setting) << " exponent " << exponent << " cell " << cell;
      }
    }
  }
}

TEST(StencilProperty, ReflectingOrTransposingTheInputMovesTheOutputWithinTheBound) {
  for (const Setting& setting : settings()) {
    const Bytes input = testing::random_grid(51, 6);
    const double m0 = testing::max_magnitude(input);
    const double bound = 2 * cell_tolerance_factor() * m0;  // two computations, two errors
    const Bytes output = run(setting, input);
    EXPECT_LE(largest_difference(run(setting, transposed(input)), transposed(output)), bound)
        << name_of(setting);
    EXPECT_LE(largest_difference(run(setting, flipped_rows(input)), flipped_rows(output)), bound)
        << name_of(setting);
  }
}

TEST(StencilProperty, ACellOnlyReachesCellsWithinEightStepsOfItself) {
  for (const Setting& setting : settings()) {
    const Bytes input = testing::random_grid(61, 7);
    const Bytes output = run(setting, input);
    for (const auto& [row, column] :
         {std::pair<std::size_t, std::size_t>{128, 128}, {20, 200}, {240, 17}, {3, 3}}) {
      Bytes disturbed = input;
      testing::set_cell(disturbed, row, column, testing::cell_of(input, row, column) / 2 + 0.25);
      const Bytes result = run(setting, disturbed);
      std::size_t reached = 0;
      for (std::size_t r = 0; r < kEdge; ++r) {
        for (std::size_t c = 0; c < kEdge; ++c) {
          const std::size_t distance =
              (r > row ? r - row : row - r) + (c > column ? c - column : column - c);
          const bool differs =
              !same_bits(testing::cell_of(output, r, c), testing::cell_of(result, r, c));
          if (distance > kSteps) {
            ASSERT_FALSE(differs) << name_of(setting) << " " << r << "," << c << " from " << row
                                  << "," << column;
          }
          reached += differs ? 1U : 0U;
        }
      }
      EXPECT_GT(reached, 0U) << "the disturbed cell itself changes the grid";
    }
  }
}

TEST(StencilProperty, ANonNegativeInputGivesANonNegativeOutput) {
  for (const Setting& setting : settings()) {
    Bytes input = testing::random_grid(71, 8);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      testing::set_cell(input, cell / kEdge, cell % kEdge,
                        std::fabs(testing::cell_of(input, cell / kEdge, cell % kEdge)));
    }
    const Bytes output = run(setting, input);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      ASSERT_GE(testing::cell_of(output, cell / kEdge, cell % kEdge), 0.0) << cell;
    }
  }
}

TEST(StencilProperty, RaisingTheInputNeverLowersTheOutput) {
  // Rounded addition and the exact halvings are monotone, so the composition is.
  for (const Setting& setting : settings()) {
    const Bytes lower = testing::random_grid(81, 9);
    Bytes higher = lower;
    Bytes bumps(kGridBytes);
    rng::fill_stream(82, 9, bumps);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      const double bump =
          static_cast<double>(std::to_integer<unsigned>(bumps[cell * 8] & std::byte{7})) * 0.01;
      testing::set_cell(higher, cell / kEdge, cell % kEdge,
                        testing::cell_of(lower, cell / kEdge, cell % kEdge) + bump);
    }
    const Bytes low_out = run(setting, lower);
    const Bytes high_out = run(setting, higher);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      ASSERT_LE(testing::cell_of(low_out, cell / kEdge, cell % kEdge),
                testing::cell_of(high_out, cell / kEdge, cell % kEdge))
          << name_of(setting) << " cell " << cell;
    }
  }
}

TEST(StencilProperty, TheEdgesOfTheValueDomainAreComputedLikeAnyOther) {
  // The input's extremes: -1 everywhere (the lower end is reached), the
  // largest generated value, zeros of both signs, and a subnormal field.
  const double largest = 1.0 - 0x1p-52;
  for (const Setting& setting : settings()) {
    for (const double value : {-1.0, largest, 0.0, -0.0, 0x1p-1074, -0x1p-1074, 0x1p-52}) {
      const Bytes input = testing::grid_of(value);
      const Bytes output = run(setting, input);
      for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
        ASSERT_TRUE(same_bits(testing::cell_of(output, cell / kEdge, cell % kEdge), value) ||
                    (value != 0.0 && std::fabs(value) < 0x1p-1000))
            << name_of(setting) << " value " << value << " cell " << cell;
      }
    }
  }
}

TEST(StencilProperty, ASubnormalFieldStaysDeterministicAndWithinItsOwnScale) {
  // Below the range the specification covers (it needs no underflow): the
  // result is whatever IEEE says, the same on every run, and still bounded.
  for (const Setting& setting : settings()) {
    Bytes input = testing::grid_of(0.0);
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      testing::set_cell(input, cell / kEdge, cell % kEdge,
                        std::ldexp(static_cast<double>(cell % 7), -1074));
    }
    const Bytes first = run(setting, input);
    EXPECT_EQ(run(setting, input), first) << name_of(setting);
    EXPECT_LE(testing::max_magnitude(first), 0x1p-1074 * 6) << name_of(setting);
  }
}

}  // namespace
}  // namespace loadforge::workloads::stencil
