// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the adaptive mesh specification carries: its constants, the
// hotspot decoder, the mesh's integer predicate, the exact pieces of a leaf's
// value, and the tolerance, checked on both sides of the bound it derives.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "workloads/adaptive_mesh/spec.hpp"

namespace loadforge::workloads::adaptive_mesh {
namespace {

constexpr double kTwoToMinus39 = 1.0 / 549755813888.0;
constexpr double kTwoToMinus41 = 1.0 / 2199023255552.0;

TEST(AdaptiveMeshSpecDefinition, TheNameAndTheSizes) {
  EXPECT_EQ(kName, "adaptive_mesh");
  EXPECT_EQ(kVersion, 1U);
  EXPECT_EQ(kBaseCells, 1U << kBaseLevel);
  EXPECT_EQ(kLattice, 1U << kMaxLevel);
  EXPECT_EQ(kCoarseCells, 256U);
  EXPECT_EQ(kHotspots, 256U);
  EXPECT_EQ(kInputBytes, 2048U);
  EXPECT_EQ(kCountsOffset, 2048U);
  EXPECT_EQ(kOutputBytes, 3072U);
  EXPECT_EQ(kMaxLeavesPerCell, 1U << (2U * (kMaxLevel - kBaseLevel)));
  EXPECT_EQ(kSampledCells, 32U);
  EXPECT_EQ(kCoarseCells % kSampledCells, 0U) << "eight coarse cells per sample";
  EXPECT_EQ(kCheckLeafCount, "leaf count of coarse cell");
  EXPECT_EQ(kCheckIntegral, "integral of coarse cell");
}

TEST(AdaptiveMeshSpecDefinition, TheConstantsOfTheArithmetic) {
  EXPECT_EQ(kSharpness, 4096.0);
  EXPECT_EQ(kUnitRoundoff, std::ldexp(1.0, -53));
  EXPECT_EQ(kToleranceSlack, 1.0 + std::ldexp(1.0, -40));
  EXPECT_EQ(kCountTolerance, 0.5);
}

TEST(AdaptiveMeshSpecDefinition, OffsetsOfIntegralsAndCounts) {
  EXPECT_EQ(integral_offset(0), 0U);
  EXPECT_EQ(integral_offset(1), 8U);
  EXPECT_EQ(integral_offset(kCoarseCells - 1), kCountsOffset - sizeof(double));
  EXPECT_EQ(count_offset(0), kCountsOffset);
  EXPECT_EQ(count_offset(1), kCountsOffset + 4U);
  EXPECT_EQ(count_offset(kCoarseCells - 1), kOutputBytes - sizeof(std::uint32_t));
}

TEST(AdaptiveMeshSpecDefinition, AHotspotIsTheTopNineBitsThenTheNextNine) {
  const Hotspot zero = hotspot_of(0);
  EXPECT_EQ(zero.x, 0U);
  EXPECT_EQ(zero.y, 0U);
  const Hotspot full = hotspot_of(~std::uint64_t{0});
  EXPECT_EQ(full.x, kLattice - 1);
  EXPECT_EQ(full.y, kLattice - 1);
  const Hotspot mixed = hotspot_of((std::uint64_t{5} << 55U) | (std::uint64_t{300} << 46U));
  EXPECT_EQ(mixed.x, 5U);
  EXPECT_EQ(mixed.y, 300U);
  const Hotspot low_bits = hotspot_of((std::uint64_t{1} << 46U) - 1);
  EXPECT_EQ(low_bits.x, 0U) << "the 46 low bits are discarded";
  EXPECT_EQ(low_bits.y, 0U);
}

TEST(AdaptiveMeshSpecDefinition, ACellContainsAHotspotWhenBothCoordinatesFallInIt) {
  const Hotspot hotspot = {300, 45};
  EXPECT_TRUE(cell_contains(Cell{0, 0, 0}, hotspot));
  EXPECT_TRUE(cell_contains(Cell{1, 1, 0}, hotspot));
  EXPECT_FALSE(cell_contains(Cell{1, 0, 0}, hotspot)) << "x in the other half";
  EXPECT_FALSE(cell_contains(Cell{1, 1, 1}, hotspot)) << "y in the other half";
  EXPECT_FALSE(cell_contains(Cell{1, 0, 1}, hotspot)) << "both in the other half";
  EXPECT_TRUE(cell_contains(Cell{kMaxLevel, 300, 45}, hotspot));
  EXPECT_FALSE(cell_contains(Cell{kMaxLevel, 301, 45}, hotspot));
  EXPECT_FALSE(cell_contains(Cell{kMaxLevel, 300, 44}, hotspot));
  EXPECT_TRUE(cell_contains(Cell{kBaseLevel, 9, 1}, hotspot)) << "300 >> 5 = 9, 45 >> 5 = 1";
}

TEST(AdaptiveMeshSpecDefinition, AHotspotOutsideTheLatticeIsInNoCellThatExists) {
  const Hotspot outside = {kLattice, 3};
  EXPECT_FALSE(cell_contains(Cell{0, 0, 0}, outside));
  for (const std::uint32_t ix : {0U, 1U}) {
    for (const std::uint32_t iy : {0U, 1U}) {
      EXPECT_FALSE(cell_contains(Cell{1, ix, iy}, outside)) << ix << ' ' << iy;
    }
  }
  const Hotspot far = {0xFFFFFFFFU, 0xFFFFFFFFU};
  EXPECT_FALSE(cell_contains(Cell{0, 0, 0}, far));
  EXPECT_FALSE(cell_contains(Cell{kMaxLevel, kLattice - 1, kLattice - 1}, far));
}

TEST(AdaptiveMeshSpecDefinition, CellCentresAreExactDyadicFractions) {
  EXPECT_EQ(cell_centre(0, 0), 0.5);
  EXPECT_EQ(cell_centre(1, 1), 0.75);
  EXPECT_EQ(cell_centre(kBaseLevel, 7), 15.0 / 32.0);
  EXPECT_EQ(cell_centre(kMaxLevel, 0), 1.0 / 1024.0);
  EXPECT_EQ(cell_centre(kMaxLevel, kLattice - 1), 1023.0 / 1024.0);
  // A coordinate far outside the lattice is still a finite number, not an overflow.
  EXPECT_EQ(cell_centre(kMaxLevel, 0xFFFFFFFFU), 8589934591.0 / 1024.0);
}

TEST(AdaptiveMeshSpecDefinition, ALeafsAreaIsAPowerOfFour) {
  EXPECT_EQ(leaf_area(0), 1.0);
  EXPECT_EQ(leaf_area(1), 0.25);
  EXPECT_EQ(leaf_area(kBaseLevel), 1.0 / 256.0);
  EXPECT_EQ(leaf_area(kMaxLevel), 1.0 / 262144.0);
}

TEST(AdaptiveMeshSpecDefinition, AHotspotsTermIsOneOverOnePlusSharpnessTimesTheSquaredDistance) {
  const Hotspot middle = {256, 256};
  const double at = cell_centre(kMaxLevel, 256);
  EXPECT_EQ(hotspot_term(Point{at, at}, middle), 1.0);
  // Neighbouring centres are 2^-9 apart: d2 = 2^-18, den = 1 + 2^-6.
  const double next = cell_centre(kMaxLevel, 257);
  EXPECT_EQ(hotspot_term(Point{next, at}, middle), 1.0 / (1.0 + 1.0 / 64.0));
  EXPECT_EQ(hotspot_term(Point{at, next}, middle), 0x1.f81f81f81f820p-1);
  // Recomputed outside C++ (Python floats, no fused operations): a level 5 leaf and the
  // hotspot at (120, 300) of the finest lattice.
  const double leaf = cell_centre(5, 7);
  EXPECT_EQ(hotspot_term(Point{leaf, leaf}, Hotspot{120, 300}), 0x1.00f7ef37ced89p-9);
}

TEST(AdaptiveMeshSpecDefinition, ATermIsAboveTwoToMinus13AtTheFarthestPoints) {
  // The farthest a leaf centre and a hotspot centre can be: opposite corners.
  const double corner = cell_centre(kMaxLevel, 0);
  EXPECT_GT(hotspot_term(Point{corner, corner}, Hotspot{kLattice - 1, kLattice - 1}), 1.0 / 8192.0);
}

TEST(AdaptiveMeshSpecDefinition, TheToleranceIsTheDerivedBoundPlusItsSlack) {
  // m = 1, c' = 1: g = 2^-52, tau = ((g / (1 - g)) * (1 + 2^-40)); recomputed outside C++.
  EXPECT_EQ(cell_tolerance(1, 1.0), 0x1.0000000001001p-52);
  EXPECT_EQ(cell_tolerance(3, 0x1.930be0ded288dp-7), 0x1.2e48e8a71f153p-57);
  EXPECT_EQ(cell_tolerance(kMaxLeavesPerCell, 0.5), 0x1.0000000001400p-43);
}

TEST(AdaptiveMeshSpecDefinition, TheToleranceScalesExactlyWithTheReference) {
  for (const std::uint32_t leaves : {1U, 2U, 7U, 100U, kMaxLeavesPerCell}) {
    EXPECT_EQ(cell_tolerance(leaves, 6.0), 2.0 * cell_tolerance(leaves, 3.0)) << leaves;
    EXPECT_EQ(cell_tolerance(leaves, 0.25), 0.25 * cell_tolerance(leaves, 1.0)) << leaves;
  }
}

TEST(AdaptiveMeshSpecDefinition,
     TheToleranceExceedsTheDerivedBoundByBetweenTwoToMinus41AndMinus39) {
  // T = g c' / (1 - g), g = 2 m u. For c' = 1: tau * (1 - g) - g is the exact excess of tau
  // over T times (1 - g); one fused operation, so it carries the sign and size of the exact
  // value.
  for (std::uint32_t leaves = 1; leaves <= kMaxLeavesPerCell; ++leaves) {
    const double g = static_cast<double>(2U * leaves) * kUnitRoundoff;
    const double den = 1.0 - g;
    const double tau = cell_tolerance(leaves, 1.0);
    const double excess = std::fma(tau, den, -g);
    EXPECT_GT(excess, g * kTwoToMinus41) << leaves;
    EXPECT_LT(excess, g * kTwoToMinus39) << leaves;
  }
}

TEST(AdaptiveMeshSpecDefinition, TheDenominatorOfTheBoundIsExact) {
  // 1 - g needs no rounding: g = m * 2^-52 and the result lies in [1/2, 1).
  for (std::uint32_t leaves = 1; leaves <= kMaxLeavesPerCell; ++leaves) {
    const double g = static_cast<double>(2U * leaves) * kUnitRoundoff;
    const double den = 1.0 - g;
    EXPECT_EQ(g, std::ldexp(static_cast<double>(leaves), -52)) << leaves;
    EXPECT_GE(den, 0.5) << leaves;
    EXPECT_EQ(1.0 - den, g) << "1 - g was rounded for " << leaves;
  }
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh
