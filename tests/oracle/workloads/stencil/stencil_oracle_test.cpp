// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the workload's output against independent readings of the
// specification, written here from spec.hpp and not from the kernels:
//   * an exact reference -- every cell after kSteps sweeps as a rational, in
//     integer arithmetic with no rounding anywhere -- against which the error
//     of every cell of the whole grid is measured, not only of four windows;
//   * the specification's own sampled-window check, scalar code sweeping each
//     grown window, which must agree with every correct output.
// The bound being tested is the derived one: 2 gamma_{4 kSteps} M0.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "unit/workloads/stencil/stencil_support.hpp"
#include "verification/oracle.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;

struct Case {
  std::uint64_t seed;
  std::uint64_t index;
  std::uint32_t tile;
};

const Case kCases[] = {{0x5EED, 0, 64},
                       {0x5EED, 1, 16},
                       {7, 3, 32},
                       {~std::uint64_t{0}, 9, 128},
                       {12345, std::uint64_t{1} << 33, 64}};

TEST(StencilOracle, EveryCellOfEveryPathIsWithinTheDerivedBoundOfTheExactValue) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    for (const Case& test : kCases) {
      const auto workload = testing::make(testing::config_for(path, test.tile, 4, test.seed));
      ASSERT_NE(workload, nullptr);
      const Bytes input = testing::generated(*workload, test.index);
      testing::SerialExecutor serial;
      const Bytes output = testing::computed(*workload, test.index, input, serial);
      const auto exact = testing::exact_after_sweeps(input);
      const testing::Deviation worst = testing::deviation_from_exact(output, exact);
      const double m0 = testing::max_magnitude(input);
      const double bound = cell_tolerance_factor() * m0;
      // The kernel's own error is at most M0 gamma_{4T}: half the bound the
      // check allows between two computations (the oracle's error is the other half).
      EXPECT_LE(worst.largest, static_cast<long double>(bound) / 2)
          << simd::describe(path) << " seed " << test.seed << " tile " << test.tile << " cell "
          << worst.cell;
    }
  }
}

TEST(StencilOracle, TheRingIsTheInputsRingAndTheExactReferenceAgreesOnIt) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  const Bytes input = testing::generated(*workload, 4);
  testing::SerialExecutor serial;
  const Bytes output = testing::computed(*workload, 4, input, serial);
  const auto exact = testing::exact_after_sweeps(input);
  for (std::size_t at = 0; at < kEdge; ++at) {
    for (const auto& [row, column] :
         {std::pair{std::size_t{0}, at}, {kEdge - 1, at}, {at, std::size_t{0}}, {at, kEdge - 1}}) {
      EXPECT_EQ(testing::cell_of(output, row, column), testing::cell_of(input, row, column));
      EXPECT_EQ(testing::exact_value(exact[row * kEdge + column]),
                static_cast<long double>(testing::cell_of(input, row, column)));
    }
  }
}

TEST(StencilOracle, AnImpulseSpreadsAsTheExactBinomialWalk) {
  // One cell of 1.0 far from the ring: every output cell is an integer
  // multiple of 8^-8 = 2^-24 (all exactly representable), they sum to 1, and
  // the central cell is the number of closed lazy walks of length 8 over 8^8.
  Bytes input = testing::grid_of(0.0);
  testing::set_cell(input, 128, 128, 1.0);
  const auto exact = testing::exact_after_sweeps(input);
  for (const simd::IsaPath path : testing::runnable_paths()) {
    const auto workload = testing::make(testing::config_for(path, 32));
    testing::SerialExecutor serial;
    const Bytes output = testing::computed(*workload, 0, input, serial);
    double sum = 0.0;
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      const double value = testing::cell_of(output, cell / kEdge, cell % kEdge);
      ASSERT_EQ(static_cast<long double>(value), testing::exact_value(exact[cell]))
          << cell << " " << simd::describe(path) << ": an exactly representable result is exact";
      sum += value;
    }
    EXPECT_EQ(sum, 1.0) << "mass is conserved away from the ring";
  }
  // The centre keeps weight: positive, and at least the 2^-8 that staying put at every sweep gives.
  EXPECT_GT(testing::exact_value(exact[128 * kEdge + 128]), 0x1p-8L);
}

TEST(StencilOracle, TheSampledWindowsAgreeWithEveryCorrectOutput) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    for (const Case& test : kCases) {
      const auto workload = testing::make(testing::config_for(path, test.tile, 4, test.seed));
      const Bytes input = testing::generated(*workload, test.index);
      testing::SerialExecutor serial;
      const Bytes output = testing::computed(*workload, test.index, input, serial);
      const verification::Verdict verdict = testing::window_check({test.index, input, output});
      const auto* agreement = std::get_if<verification::Agreement>(&verdict);
      ASSERT_NE(agreement, nullptr) << simd::describe(path) << " tile " << test.tile;
      EXPECT_EQ(agreement->blocks_compared, kCellsChecked);
      EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
    }
  }
}

TEST(StencilOracle, TheWindowsAreTheTwoCornersAndTwoThatDependOnTheIndexAlone) {
  EXPECT_EQ(testing::window_origin(5, 0), (std::pair<std::size_t, std::size_t>{0, 0}));
  EXPECT_EQ(testing::window_origin(5, 1),
            (std::pair<std::size_t, std::size_t>{kEdge - kRegionEdge, kEdge - kRegionEdge}));
  for (std::uint64_t index = 0; index < 200; ++index) {
    for (const std::size_t window : {std::size_t{2}, std::size_t{3}}) {
      const auto [row, column] = testing::window_origin(index, window);
      EXPECT_LE(row, kEdge - kRegionEdge);
      EXPECT_LE(column, kEdge - kRegionEdge);
      EXPECT_EQ(testing::window_origin(index, window), std::make_pair(row, column));
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::stencil
