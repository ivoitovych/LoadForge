// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 of the adaptive mesh oracle. There is no reference tool for a synthetic
// mesh, so the reference is exact arithmetic: every x_leaf is a multiple of
// 2^-75 below 2^9, so a coarse cell's exact integral E is held in an unsigned
// 128-bit integer scaled by 2^75, and nothing rounds. Against it:
//
//   * the specification's step 1 and 2: the leaf-order sum c' is within
//     gamma_(m-1) E of E, on generated units, on every coarse cell;
//   * the oracle's tolerance covers the other orders a stress run could add
//     the leaves in (reverse, pairwise): the oracle agrees with an output
//     built from each, and the residual is below tau;
//   * the mesh known by hand: one hotspot makes 16 leaves in its coarse cell
//     and none anywhere else; the spec's figures for a generated unit
//     (about 3700 leaves, at most about 50 in a cell).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "unit/verification/oracle/adaptive_mesh/mesh_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/adaptive_mesh/oracle.hpp"
#include "workloads/adaptive_mesh/spec.hpp"

namespace loadforge::verification::oracle::adaptive_mesh {
namespace {

namespace spec = workloads::adaptive_mesh;
namespace mesh = test::mesh;

__extension__ using Fixed = unsigned __int128;
constexpr int kScale = 75;

/// x as a multiple of 2^-75; x is positive, at least 2^-23 and below 2^9.
Fixed fixed_of(double x) {
  int exponent = 0;
  const double fraction = std::frexp(x, &exponent);
  const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
  EXPECT_EQ(std::ldexp(static_cast<double>(mantissa), exponent - 53), x);
  const int shift = exponent - 53 + kScale;
  EXPECT_GE(shift, 0);
  return static_cast<Fixed>(mantissa) << shift;
}

long double ld_of(Fixed value) { return std::ldexp(static_cast<long double>(value), -kScale); }

long double gamma(std::size_t k) {
  const long double ku = static_cast<long double>(k) * std::ldexp(1.0L, -53);
  return ku / (1.0L - ku);
}

Fixed exact_integral(const std::vector<mesh::Leaf>& leaves) {
  Fixed total = 0;
  for (const mesh::Leaf& leaf : leaves) {
    total += fixed_of(leaf.value);
  }
  return total;
}

TEST(AdaptiveMeshExact, TheLeafOrderSumIsWithinTheHighamBoundOfTheExactIntegral) {
  std::size_t cells_with_rounding = 0;
  for (std::uint64_t index = 0; index < 4; ++index) {
    const mesh::Model model = mesh::model_of(mesh::generated(21, index));
    for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
      const std::size_t m = model.leaves[c].size();
      const Fixed exact = exact_integral(model.leaves[c]);
      const Fixed sum = fixed_of(model.reference[c]);
      const long double error = ld_of(sum > exact ? sum - exact : exact - sum);
      EXPECT_LE(error, gamma(m - 1) * ld_of(exact)) << "unit " << index << " cell " << c;
      cells_with_rounding += sum != exact ? 1 : 0;
    }
  }
  EXPECT_GT(cells_with_rounding, 0U) << "some sum should round, or the bound is untested";
}

TEST(AdaptiveMeshExact, OtherOrdersOfAddingStayInsideTheTolerance) {
  const mesh::Hotspots hotspots = mesh::crowded(37);
  const mesh::Model model = mesh::model_of(hotspots);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  const std::size_t m = model.leaves[37].size();
  ASSERT_EQ(m, spec::kMaxLeavesPerCell);
  const long double exact = ld_of(exact_integral(model.leaves[37]));
  const double reference = model.reference[37];
  const double tolerance = spec::cell_tolerance(static_cast<std::uint32_t>(m), reference);
  std::uint64_t index = 0;
  const auto samples = [](std::uint64_t unit) {
    for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
      if (sampled_cell(unit, j) == 37) {
        return true;
      }
    }
    return false;
  };
  while (!samples(index)) {
    ASSERT_LT(++index, 1000U) << "no index samples cell 37";
  }
  for (const double other :
       {mesh::reverse_order_sum(model.leaves[37]), mesh::pairwise_order_sum(model.leaves[37])}) {
    // Step 3: |c - c'| <= 2 gamma_(m-1) E, and tau covers it.
    EXPECT_LE(std::fabs(static_cast<long double>(other) - reference), 2 * gamma(m - 1) * exact);
    EXPECT_LE(std::fabs(other - reference), tolerance);
    mesh::Bytes output = mesh::render(model);
    mesh::put_double(output, spec::integral_offset(37), other);
    const AdaptiveMeshOracle oracle;
    EXPECT_TRUE(
        std::holds_alternative<Agreement>(oracle.check(UnitEvidence{index, input, output})));
  }
}

TEST(AdaptiveMeshExact, OneHotspotMakesSixteenLeavesInItsCellAndOneInEveryOther) {
  // A leaf per level below the base: 3 siblings at each of the 5 levels, and
  // the finest cell itself: 3 * 5 + 1.
  const mesh::Model model = mesh::model_of(mesh::all_at(100, 300));
  const std::size_t hot = (300 >> 5) * spec::kBaseCells + (100 >> 5);
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    EXPECT_EQ(model.leaves[c].size(), c == hot ? 16U : 1U) << c;
  }
  // Away from the hotspot the cell is one leaf, f = 256 terms (rounded as
  // they are added), times 4^-4.
  const std::size_t far = 0;
  const long double point_x = 1.0L / 32.0L;
  const long double dx = point_x - (100.5L / 512.0L);
  const long double dy = point_x - (300.5L / 512.0L);
  const long double term = 1.0L / (1.0L + 4096.0L * (dx * dx + dy * dy));
  const long double by_hand = 256.0L * term / 256.0L;
  EXPECT_NEAR(static_cast<double>(model.reference[far] / by_hand), 1.0, 1e-13);
  const mesh::Bytes input = mesh::bytes_of(mesh::all_at(100, 300));
  const AdaptiveMeshOracle oracle;
  EXPECT_TRUE(
      std::holds_alternative<Agreement>(oracle.check(UnitEvidence{0, input, mesh::render(model)})));
}

TEST(AdaptiveMeshExact, AGeneratedUnitHasAboutThirtySevenHundredLeavesAndFewInAnyCell) {
  for (std::uint64_t index = 0; index < 4; ++index) {
    const mesh::Model model = mesh::model_of(mesh::generated(21, index));
    std::size_t total = 0;
    std::size_t most = 0;
    for (const auto& leaves : model.leaves) {
      total += leaves.size();
      most = std::max(most, leaves.size());
    }
    EXPECT_GT(total, 3000U);
    EXPECT_LT(total, 4400U);
    EXPECT_LE(most, 80U);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::adaptive_mesh

// The contract header registers two suites; this binary runs neither (the
// oracle's own suites do, in tests/unit).
namespace loadforge::contracts {
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
