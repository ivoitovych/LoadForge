// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b: corruption introduced on purpose into the workload's evidence,
// and the classification the specification's sampled-window check produces
// for each; and the evidence it cannot judge, which must come back as "not
// checked" and never as agreement (verification.md §8).
//
// The check used is the one written in the tests from spec.hpp
// (stencil_support.hpp), the specification's own procedure: the independent
// oracle of the oracle stream is a different reading of the same text.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <variant>

#include "unit/workloads/stencil/stencil_support.hpp"
#include "verification/oracle.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;
using verification::Agreement;
using verification::BoundedMismatch;
using verification::NotChecked;
using verification::Verdict;

constexpr std::uint64_t kIndex = 3;

struct Unit {
  Bytes input;
  Bytes output;
};

Unit healthy_unit(simd::IsaPath path = simd::IsaPath::kScalar) {
  const auto workload = testing::make(testing::config_for(path, 0));
  Unit unit;
  unit.input = testing::generated(*workload, kIndex);
  testing::SerialExecutor serial;
  unit.output = testing::computed(*workload, kIndex, unit.input, serial);
  return unit;
}

Verdict check(const Unit& unit) { return testing::window_check({kIndex, unit.input, unit.output}); }

BoundedMismatch mismatch_of(const Verdict& verdict) {
  const auto* mismatch = std::get_if<BoundedMismatch>(&verdict);
  EXPECT_NE(mismatch, nullptr);
  return mismatch != nullptr ? *mismatch : BoundedMismatch{};
}

void flip_bit(Bytes& grid, std::size_t row, std::size_t column, unsigned bit) {
  grid[(row * kEdge + column) * 8 + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

TEST(StencilFault, ACleanUnitIsAgreementOnEveryCheckedCell) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    const Verdict verdict = check(healthy_unit(path));
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << simd::describe(path);
    EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, kCellsChecked);
  }
}

TEST(StencilFault, AFlippedBitInAWindowIsABoundedMismatchNamingTheCell) {
  const Unit clean = healthy_unit();
  const auto [row2, column2] = testing::window_origin(kIndex, 2);
  const auto [row3, column3] = testing::window_origin(kIndex, 3);
  struct Where {
    std::size_t row;
    std::size_t column;
    std::uint64_t compared;  // cells compared up to and including the corrupt one
  };
  const Where places[] = {
      {0, 0, 1},                                              // window 0, first cell
      {5, 9, 5 * kRegionEdge + 9 + 1},                        // window 0, inside
      {kEdge - 1, kEdge - 1, 2 * kRegionEdge * kRegionEdge},  // window 1, last cell
      {kEdge - kRegionEdge, kEdge - kRegionEdge, kRegionEdge * kRegionEdge + 1},  // window 1, first
      {row2 + 7, column2 + 3, 2 * kRegionEdge * kRegionEdge + 7 * kRegionEdge + 3 + 1},
      {row3 + 15, column3 + 15, 4 * kRegionEdge * kRegionEdge},
  };
  for (const unsigned bit : {62U, 51U, 40U, 30U}) {
    for (const Where& where : places) {
      Unit unit = clean;
      flip_bit(unit.output, where.row, where.column, bit);
      const Verdict verdict = check(unit);
      const BoundedMismatch mismatch = mismatch_of(verdict);
      // A cell lying in two windows is named by the first of them in order, so
      // compare the cell and not the count when windows overlap.
      EXPECT_EQ(mismatch.check, "stencil-cell");
      ASSERT_TRUE(mismatch.element.has_value());
      EXPECT_TRUE(mismatch.residual > mismatch.tolerance || std::isnan(mismatch.residual));
      EXPECT_GT(mismatch.tolerance, 0.0);
      EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
      if (*mismatch.element == where.row * kEdge + where.column) {
        EXPECT_LE(mismatch.blocks_compared, where.compared)
            << "bit " << bit << " at " << where.row << "," << where.column;
      }
    }
  }
}

TEST(StencilFault, TheFirstFailingCellInWindowOrderIsTheOneNamed) {
  const Unit clean = healthy_unit();
  const auto [row2, column2] = testing::window_origin(kIndex, 2);
  Unit unit = clean;
  flip_bit(unit.output, row2 + 1, column2, 50);     // window 2
  flip_bit(unit.output, kEdge - 1, kEdge - 1, 50);  // window 1
  flip_bit(unit.output, 0, 15, 50);                 // window 0, later in its row
  flip_bit(unit.output, 1, 0, 50);                  // window 0, the next row, earlier column
  BoundedMismatch mismatch = mismatch_of(check(unit));
  EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>{15}) << "row-major inside the window";
  EXPECT_EQ(mismatch.blocks_compared, 16U);
  // Without the window 0 faults the window 1 cell is the first.
  unit = clean;
  flip_bit(unit.output, row2 + 1, column2, 50);
  flip_bit(unit.output, kEdge - 1, kEdge - 1, 50);
  mismatch = mismatch_of(check(unit));
  EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>{(kEdge - 1) * kEdge + kEdge - 1});
  EXPECT_EQ(mismatch.blocks_compared, 2U * kRegionEdge * kRegionEdge);
}

TEST(StencilFault, ANotANumberOrAnInfinityInTheOutputIsAMismatchNotAnUnrunCheck) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    Unit unit = healthy_unit();
    testing::set_cell(unit.output, 2, 2, bad);
    const Verdict verdict = check(unit);
    const BoundedMismatch mismatch = mismatch_of(verdict);
    EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>{2 * kEdge + 2});
    EXPECT_TRUE(!(mismatch.residual <= mismatch.tolerance));
    EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
  }
}

TEST(StencilFault, ADisturbanceBelowTheToleranceGoesUnseenAndOneAboveItIsCaught) {
  const Unit clean = healthy_unit();
  const double m0 = testing::max_magnitude(clean.input);
  const double tolerance = testing::window_tolerance(m0);
  for (const double factor : {0.25, 0.5}) {
    Unit unit = clean;
    testing::set_cell(unit.output, 8, 8, testing::cell_of(clean.output, 8, 8) + factor * tolerance);
    EXPECT_TRUE(std::holds_alternative<Agreement>(check(unit))) << factor;
  }
  for (const double factor : {2.0, 4.0}) {
    for (const double sign : {1.0, -1.0}) {
      Unit unit = clean;
      testing::set_cell(unit.output, 8, 8,
                        testing::cell_of(clean.output, 8, 8) + sign * factor * tolerance);
      const BoundedMismatch mismatch = mismatch_of(check(unit));
      EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>{8 * kEdge + 8}) << factor;
    }
  }
}

TEST(StencilFault, ACorruptRingCellInsideAWindowIsCaughtAndOutsideAnyWindowIsNot) {
  Unit unit = healthy_unit();
  const Bytes clean_output = unit.output;
  flip_bit(unit.output, 0, 0, 45);
  EXPECT_EQ(mismatch_of(check(unit)).element, std::optional<std::uint64_t>{0});
  // A ring cell no window covers: the specification does not examine it
  // (the workload's own tests do: the ring is the input's, bit for bit).
  unit.output = clean_output;
  const auto windows = [&](std::size_t row, std::size_t column) {
    for (std::size_t window = 0; window < kRegions; ++window) {
      const auto [r, c] = testing::window_origin(kIndex, window);
      if (row >= r && row < r + kRegionEdge && column >= c && column < c + kRegionEdge) {
        return true;
      }
    }
    return false;
  };
  std::size_t column = 100;
  while (windows(0, column)) {
    ++column;
  }
  flip_bit(unit.output, 0, column, 45);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(unit)));
}

TEST(StencilFault, AFlipInAnyInteriorCellOfTheOutputOutsideTheWindowsIsNotExamined) {
  Unit unit = healthy_unit();
  flip_bit(unit.output, 100, 100, 62);  // far from the corners; windows 2, 3 may or may not be near
  const auto [row2, column2] = testing::window_origin(kIndex, 2);
  const auto [row3, column3] = testing::window_origin(kIndex, 3);
  const bool in_window =
      (100 >= row2 && 100 < row2 + kRegionEdge && 100 >= column2 && 100 < column2 + kRegionEdge) ||
      (100 >= row3 && 100 < row3 + kRegionEdge && 100 >= column3 && 100 < column3 + kRegionEdge);
  EXPECT_EQ(std::holds_alternative<Agreement>(check(unit)), !in_window);
}

TEST(StencilFault, ACorruptedInputIsFollowedThroughTheComputationAndSeenAgainstTheOriginal) {
  // The event the tool exists to see: the input is damaged after it was
  // generated, the unit is computed from the damaged copy, and the check
  // is given the input the unit should have had.
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, 0));
  const Bytes original = testing::generated(*workload, kIndex);
  testing::SerialExecutor serial;
  const auto reach = [&](std::size_t row, std::size_t column, unsigned bit) {
    Bytes damaged = original;
    flip_bit(damaged, row, column, bit);
    const Bytes output = testing::computed(*workload, kIndex, damaged, serial);
    return testing::window_check({kIndex, original, output});
  };
  // Inside window 0's dependency cone (within eight cells of its corner).
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(reach(3, 3, 51)));
  // Far from every window's cone: the damage is real and the check cannot see it.
  const auto [row2, column2] = testing::window_origin(kIndex, 2);
  const auto [row3, column3] = testing::window_origin(kIndex, 3);
  std::size_t row = 128;
  std::size_t column = 128;
  const auto near_window = [&](std::size_t r, std::size_t c, std::size_t origin_row,
                               std::size_t origin_column) {
    return r + kSteps >= origin_row && r < origin_row + kRegionEdge + kSteps &&
           c + kSteps >= origin_column && c < origin_column + kRegionEdge + kSteps;
  };
  while (near_window(row, column, row2, column2) || near_window(row, column, row3, column3)) {
    row = (row + 40) % 200 + 20;
    column = (column + 37) % 200 + 20;
  }
  EXPECT_TRUE(std::holds_alternative<Agreement>(reach(row, column, 51)));
}

TEST(StencilFault, AMismatchOfAnyKindIsAValidVerdictOfABoundedOracle) {
  Unit unit = healthy_unit();
  flip_bit(unit.output, 4, 4, 55);
  const Verdict verdict = check(unit);
  EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
  EXPECT_FALSE(verification::is_valid(verdict, verification::Contract::kExact))
      << "a bounded oracle never answers with the exact kind";
}

// ---- T5b: the check that could not be made -------------------------------------

void expect_not_checked(const Verdict& verdict, const std::string& because) {
  const auto* unchecked = std::get_if<NotChecked>(&verdict);
  ASSERT_NE(unchecked, nullptr) << because;
  EXPECT_EQ(unchecked->blocks_compared, 0U) << because;
  EXPECT_FALSE(unchecked->what.empty());
  EXPECT_FALSE(unchecked->why.empty());
  EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded)) << because;
}

TEST(StencilUnrunCheck, EvidenceOfTheWrongSizeIsNotChecked) {
  const Unit unit = healthy_unit();
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{1}, kGridBytes - 1, kGridBytes + 1, 2 * kGridBytes}) {
    const Bytes wrong(size);
    expect_not_checked(testing::window_check({kIndex, wrong, unit.output}),
                       "input of " + std::to_string(size));
    expect_not_checked(testing::window_check({kIndex, unit.input, wrong}),
                       "output of " + std::to_string(size));
  }
}

TEST(StencilUnrunCheck, AnInputWithoutADerivableToleranceIsNotChecked) {
  const Unit clean = healthy_unit();
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 1.0000000000000002, -1.0000000000000002, 1e300}) {
    Unit unit = clean;
    testing::set_cell(unit.input, 77, 78, bad);
    expect_not_checked(check(unit), std::to_string(bad));
  }
}

TEST(StencilUnrunCheck, AnInputOfMagnitudeExactlyOneIsChecked) {
  Unit unit = healthy_unit();
  testing::set_cell(unit.input, 77, 78, -1.0);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check(unit)));
  testing::set_cell(unit.input, 77, 78, 1.0);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check(unit)));
}

TEST(StencilUnrunCheck, AnAllZeroInputIsNotCheckedBecauseNoToleranceIsPositive) {
  Unit unit;
  unit.input = testing::grid_of(0.0);
  unit.output = testing::grid_of(0.0);
  expect_not_checked(check(unit), "all zero");
  testing::set_cell(unit.input, 5, 5, 0x1p-1074);  // one non-zero cell is enough to derive T
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check(unit)));
}

TEST(StencilUnrunCheck, ANotANumberInTheOutputIsNeverTheUnrunCheck) {
  Unit unit = healthy_unit();
  testing::set_cell(unit.output, 0, 0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check(unit)));
}

}  // namespace
}  // namespace loadforge::workloads::stencil
