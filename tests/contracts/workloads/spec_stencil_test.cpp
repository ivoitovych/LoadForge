// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the stencil specification carries.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "workloads/stencil/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

TEST(StencilSpecDefinition, TheNameIsAValidWorkloadNameAndTheSizesAreTheUnitsLayout) {
  EXPECT_EQ(stencil::kName, "stencil");
  EXPECT_TRUE(is_valid_name(stencil::kName));
  EXPECT_EQ(stencil::kVersion, 1U);
  EXPECT_EQ(stencil::kEdge, 256U);
  EXPECT_EQ(stencil::kSteps, 8U);
  EXPECT_EQ(stencil::kGridBytes, 256U * 256U * 8U);
  EXPECT_EQ(stencil::kInputBytes, 524288U);
  EXPECT_EQ(stencil::kOutputBytes, stencil::kInputBytes);
}

TEST(StencilSpecDefinition, CellsAreExactMultiplesOfTwoToMinus52InTheHalfOpenInterval) {
  using stencil::entry_value;
  EXPECT_EQ(entry_value(0), -1.0) << "the lower end is reached";
  EXPECT_EQ(entry_value(std::uint64_t{1} << 63U), 0.0);
  EXPECT_EQ(entry_value(~std::uint64_t{0}), 1.0 - 1.0 / 4503599627370496.0)
      << "the upper end, +1, is not";
  EXPECT_EQ(entry_value(std::uint64_t{1} << 11U), -1.0 + 1.0 / 4503599627370496.0);
  EXPECT_EQ(entry_value(0x7FFU), -1.0) << "the low 11 bits are discarded";
}

TEST(StencilSpecDefinition, TileEdgesDivideTheGridAndTheDefaultIsOne) {
  for (const std::uint32_t edge : {16U, 32U, 64U, 128U}) {
    EXPECT_TRUE(stencil::is_tile_edge(edge)) << edge;
    EXPECT_EQ(stencil::kEdge % edge, 0U) << edge;
  }
  for (const std::uint32_t edge : {0U, 1U, 8U, 48U, 256U, 512U}) {
    EXPECT_FALSE(stencil::is_tile_edge(edge)) << edge;
  }
  EXPECT_TRUE(stencil::is_tile_edge(stencil::kDefaultTileEdge));
  EXPECT_EQ(stencil::kDefaultTileEdge, 64U);
}

TEST(StencilSpecDefinition, TheRingIsTheFourOuterRowsAndColumns) {
  using stencil::is_ring;
  const std::size_t last = stencil::kEdge - 1;
  for (const std::size_t at : {std::size_t{0}, std::size_t{100}, last}) {
    EXPECT_TRUE(is_ring(0, at)) << at;
    EXPECT_TRUE(is_ring(last, at)) << at;
    EXPECT_TRUE(is_ring(at, 0)) << at;
    EXPECT_TRUE(is_ring(at, last)) << at;
  }
  EXPECT_FALSE(is_ring(1, 1));
  EXPECT_FALSE(is_ring(last - 1, last - 1));
  EXPECT_FALSE(is_ring(128, 128));
  std::size_t ring = 0;
  for (std::size_t i = 0; i < stencil::kEdge; ++i) {
    for (std::size_t j = 0; j < stencil::kEdge; ++j) {
      ring += static_cast<std::size_t>(is_ring(i, j));
    }
  }
  EXPECT_EQ(ring, 4U * (stencil::kEdge - 1));
}

TEST(StencilSpecDefinition, TheWindowsFitTheGridAndCountTheCellsChecked) {
  EXPECT_EQ(stencil::kRegions, 4U);
  EXPECT_EQ(stencil::kRegionEdge, 16U);
  EXPECT_EQ(stencil::kCellsChecked, 4U * 16U * 16U);
  EXPECT_LE(stencil::kRegionEdge + 2 * stencil::kSteps, stencil::kEdge);
}

TEST(StencilSpecDefinition, GammaIsKuOverOneMinusKu) {
  EXPECT_EQ(stencil::kUnitRoundoff, std::ldexp(1.0, -53));
  EXPECT_EQ(stencil::gamma(0), 0.0);
  const double ku = 32.0 * stencil::kUnitRoundoff;
  EXPECT_EQ(stencil::gamma(32), ku / (1.0 - ku));
  EXPECT_GT(stencil::gamma(32), 32.0 * stencil::kUnitRoundoff);
}

TEST(StencilSpecDefinition, TheCellToleranceFactorIsTwoGammaOfFourStepsAndTheSlackIs2To36) {
  EXPECT_EQ(stencil::cell_tolerance_factor(), 2.0 * stencil::gamma(4 * stencil::kSteps));
  EXPECT_NEAR(stencil::cell_tolerance_factor(), 7.1054273576e-15, 1e-24);
  EXPECT_EQ(stencil::kEvaluationSlack, std::ldexp(1.0, -36));
}

}  // namespace
}  // namespace loadforge::workloads
