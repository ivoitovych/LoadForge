// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b of the adaptive mesh oracle (verification.md §8; IMPLEMENTATION
// §2).
//
// T5: a corrupted output is caught, as the bounded kind of mismatch.
//   * every bit of every leaf count, one at a time: a BoundedMismatch naming
//     the cell, with the residual the flip makes and the tolerance 0.5;
//   * every bit of every sampled integral, one at a time: the flip moves the
//     double by an amount the test works out for itself (in extended
//     precision, from the unit's own model); a flip inside tau is a result
//     the specification calls right and is agreed with, one outside it is a
//     BoundedMismatch whose residual is that amount (a single flip does not
//     make a NaN or an infinity of a finite integral of this size; the unit
//     tests give those directly) -- so the low bits of an integral, below the
//     tolerance, are the ones nobody can see, and the oracle says so by
//     agreeing;
//   * the integral of a cell nobody samples is not looked at;
//   * the bits of the input: a coordinate pushed to 512 or beyond is
//     NotChecked, and one that stays on the lattice is judged against the
//     output as it is.
//
// T5b: a check that cannot be made is NotChecked, never an Agreement --
// evidence of the wrong size, a hotspot off the lattice, and a plan that is
// not an adaptive mesh unit's (no oracle at all).

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "unit/verification/oracle/adaptive_mesh/mesh_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/adaptive_mesh/oracle.hpp"
#include "verification/record.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::adaptive_mesh {
namespace {

namespace spec = workloads::adaptive_mesh;
namespace mesh = test::mesh;

constexpr std::uint64_t kIndex = 19;

struct Unit {
  mesh::Hotspots hotspots;
  mesh::Model model;
  mesh::Bytes input;
  mesh::Bytes output;
};

const Unit& unit() {
  static const Unit kUnit = [] {
    const mesh::Hotspots hotspots = mesh::generated(91, kIndex);
    const mesh::Model model = mesh::model_of(hotspots);
    return Unit{hotspots, model, mesh::bytes_of(hotspots), mesh::render(model)};
  }();
  return kUnit;
}

Verdict judge(const mesh::Bytes& input, const mesh::Bytes& output) {
  return AdaptiveMeshOracle().check(UnitEvidence{kIndex, input, output});
}

void flip(mesh::Bytes& bytes, std::size_t bit) {
  bytes[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

BoundedMismatch bounded_of(const Verdict& verdict) {
  const auto* bounded = std::get_if<BoundedMismatch>(&verdict);
  EXPECT_NE(bounded, nullptr);
  return bounded != nullptr ? *bounded : BoundedMismatch{"none", 0.0, 1.0, std::nullopt, 0, 1};
}

TEST(AdaptiveMeshCorruption, TheUncorruptedUnitIsAgreedWith) {
  ASSERT_TRUE(std::holds_alternative<Agreement>(judge(unit().input, unit().output)));
}

TEST(AdaptiveMeshCorruption, EveryBitOfEveryLeafCountIsCaughtWithTheResidualItMakes) {
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    for (std::size_t bit = 0; bit < 32; ++bit) {
      mesh::Bytes output = unit().output;
      flip(output, spec::count_offset(c) * 8 + bit);
      const Verdict verdict = judge(unit().input, output);
      const BoundedMismatch found = bounded_of(verdict);
      EXPECT_EQ(found.check, std::string(spec::kCheckLeafCount) + " " + std::to_string(c));
      EXPECT_EQ(found.residual, static_cast<double>(std::uint64_t{1} << bit));
      EXPECT_EQ(found.tolerance, spec::kCountTolerance);
      EXPECT_EQ(found.element, std::optional<std::uint64_t>(c));
      EXPECT_FALSE(found.condition_number.has_value());
      EXPECT_EQ(found.blocks_compared, c + 1);
      EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
    }
  }
}

TEST(AdaptiveMeshCorruption, EveryBitOfEverySampledIntegralIsJudgedAgainstTheToleranceOfItsCell) {
  std::size_t agreed = 0;
  std::size_t caught = 0;
  for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
    const std::size_t c = sampled_cell(kIndex, j);
    const double reference = unit().model.reference[c];
    const double tolerance =
        spec::cell_tolerance(static_cast<std::uint32_t>(unit().model.leaves[c].size()), reference);
    for (std::size_t bit = 0; bit < 64; ++bit) {
      mesh::Bytes output = unit().output;
      flip(output, spec::integral_offset(c) * 8 + bit);
      const double stored = mesh::get_double(output, spec::integral_offset(c));
      const long double moved =
          std::fabs(static_cast<long double>(stored) - static_cast<long double>(reference));
      const Verdict verdict = judge(unit().input, output);
      EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
      if (std::isfinite(stored) && moved <= static_cast<long double>(tolerance) * (1 - 1e-6L)) {
        ++agreed;
        EXPECT_TRUE(std::holds_alternative<Agreement>(verdict)) << "cell " << c << " bit " << bit;
      } else if (!std::isfinite(stored) ||
                 moved > static_cast<long double>(tolerance) * (1 + 1e-6L)) {
        ++caught;
        const BoundedMismatch found = bounded_of(verdict);
        EXPECT_EQ(found.check, std::string(spec::kCheckIntegral) + " " + std::to_string(c));
        EXPECT_EQ(found.element, std::optional<std::uint64_t>(c));
        EXPECT_EQ(found.blocks_compared, spec::kCoarseCells + j + 1);
        EXPECT_EQ(found.tolerance, tolerance);
        if (std::isfinite(stored)) {
          EXPECT_EQ(found.residual, std::fabs(stored - reference));
        } else {
          EXPECT_FALSE(found.residual <= found.tolerance);
        }
      }
      // A flip landing within a millionth of the tolerance is neither asserted.
    }
  }
  EXPECT_GT(agreed, 0U) << "the low bits of an integral lie inside the tolerance";
  EXPECT_GT(caught, spec::kSampledCells * 20);
}

TEST(AdaptiveMeshCorruption, AnIntegralOfACellNobodySamplesIsNotLookedAt) {
  std::set<std::size_t> sampled;
  for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
    sampled.insert(sampled_cell(kIndex, j));
  }
  std::size_t looked_at = 0;
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    if (sampled.contains(c)) {
      continue;
    }
    for (const std::size_t bit : {0U, 30U, 52U, 62U, 63U}) {
      mesh::Bytes output = unit().output;
      flip(output, spec::integral_offset(c) * 8 + bit);
      looked_at += std::holds_alternative<Agreement>(judge(unit().input, output)) ? 0 : 1;
    }
  }
  EXPECT_EQ(looked_at, 0U);
}

TEST(AdaptiveMeshCorruption, ABitOfTheInputIsNotCheckedOffTheLatticeAndJudgedOnIt) {
  std::size_t not_checked = 0;
  std::size_t judged = 0;
  std::size_t mismatches = 0;
  for (std::size_t hotspot = 0; hotspot < spec::kHotspots; hotspot += 5) {
    for (std::size_t bit = 0; bit < spec::kHotspotBytes * 8; ++bit) {
      mesh::Bytes input = unit().input;
      flip(input, hotspot * spec::kHotspotBytes * 8 + bit);
      const Verdict verdict = judge(input, unit().output);
      EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
      const bool off_lattice = bit % 32 >= 9;
      if (off_lattice) {
        ++not_checked;
        ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << hotspot << " " << bit;
        EXPECT_EQ(blocks_of(verdict), 0U);
      } else {
        ++judged;
        ASSERT_FALSE(std::holds_alternative<NotChecked>(verdict)) << hotspot << " " << bit;
        mismatches += std::holds_alternative<BoundedMismatch>(verdict) ? 1 : 0;
      }
    }
  }
  EXPECT_GT(not_checked, 0U);
  EXPECT_GT(judged, 0U);
  EXPECT_GT(mismatches, 0U) << "moving a hotspot changes the leaf counts the output was built from";
}

// T5b.

TEST(AdaptiveMeshNotChecked, EvidenceOfAnyOtherSizeComparesNothing) {
  const std::vector<std::pair<mesh::Bytes, mesh::Bytes>> sizes = {
      {mesh::Bytes(spec::kInputBytes + 1), unit().output},
      {unit().input, mesh::Bytes(spec::kOutputBytes - 1)},
      {mesh::Bytes(spec::kInputBytes - 8), mesh::Bytes(spec::kOutputBytes + 8)},
      {{}, {}},
  };
  for (const auto& [input, output] : sizes) {
    const Verdict verdict = judge(input, output);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    EXPECT_EQ(blocks_of(verdict), 0U);
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  }
}

TEST(AdaptiveMeshNotChecked, AHotspotOffTheLatticeComparesNothingEvenOverACorruptOutput) {
  mesh::Bytes corrupt = unit().output;
  mesh::put32(corrupt, spec::count_offset(0), 12345);
  for (const std::uint32_t bad : {512U, 1000U, 0xFFFFFFFFU}) {
    for (const std::size_t hotspot : {0U, 100U, 255U}) {
      mesh::Hotspots hotspots = unit().hotspots;
      hotspots[hotspot].y = bad;
      const Verdict verdict = judge(mesh::bytes_of(hotspots), corrupt);
      ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
      EXPECT_EQ(blocks_of(verdict), 0U);
    }
  }
}

TEST(AdaptiveMeshNotChecked, APlanThatIsNotAnAdaptiveMeshUnitGivesNoOracleAtAll) {
  const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, 4, 0, 0};
  const auto refused = create(workloads::WorkloadConfig{}, plan);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, OracleFault::kConfiguration);
}

}  // namespace
}  // namespace loadforge::verification::oracle::adaptive_mesh

namespace loadforge::contracts {
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
