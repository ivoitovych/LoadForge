// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the sparse oracle. T5: corruption is introduced into the
// output of a unit scipy solved, and the verdict is asserted for which check
// fired, which row it named and how many checks had been made. T5b: the check
// is made impossible, and the verdict must be NotChecked with nothing
// compared, never an agreement.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "oracle/verification/oracle/sparse/sparse_evidence.hpp"
#include "verification/oracle/sparse/sparse_oracle.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::verification::oracle::sparse {
namespace {

namespace spec = workloads::sparse;
using evidence::System;
using evidence::Unit;

const Unit& good_unit() {
  static const Unit unit = evidence::real_tool_unit();
  return unit;
}

std::unique_ptr<Oracle> make_oracle() {
  auto created = create(
      workloads::WorkloadConfig{},
      workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, spec::kDefaultChunkRows, 0, 0});
  return std::move(created).value();
}

Verdict check_unit(const Unit& unit) {
  const auto oracle = make_oracle();
  return oracle->check(UnitEvidence{unit.index, unit.input, unit.output});
}

void flip(std::vector<std::byte>& bytes, std::size_t offset, unsigned bit) {
  bytes[offset + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

void put_double(std::vector<std::byte>& bytes, std::size_t offset, double value) {
  const std::vector<std::byte> encoded = evidence::bytes_of(std::vector<double>(1, value));
  std::copy(encoded.begin(), encoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void put_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t word) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    bytes[offset + byte] = static_cast<std::byte>((word >> (8U * byte)) & 0xFFU);
  }
}

constexpr std::size_t kIterateAt = 0;
constexpr std::size_t kResidualAt = spec::kRows * sizeof(double);

BoundedMismatch mismatch_of(const Verdict& verdict) {
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  return std::get<BoundedMismatch>(verdict);
}

/// The first row whose residual involves column `column`: a row holds an entry
/// at the column when the column is within an offset of it.
std::size_t first_row_touching(std::size_t column) {
  std::size_t first = column;
  for (const std::size_t offset : spec::kOffsets) {
    if (column >= offset) {
      first = std::min(first, column - offset);
    }
  }
  return first;
}

TEST(SparseFaultInjection, TheUncorruptedUnitIsAgreedWith) {
  const Verdict verdict = check_unit(good_unit());
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kRows + 1);
}

TEST(SparseFaultInjection, AnExponentFlipInTheIterateIsNamedAtTheFirstRowThatReadsIt) {
  for (const std::size_t column :
       {std::size_t{0}, std::size_t{500}, std::size_t{5000}, spec::kRows - 1}) {
    Unit unit = good_unit();
    flip(unit.output, kIterateAt + column * 8, 62);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "residual-row");
    const std::size_t row = first_row_touching(column);
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(row)) << "column " << column;
    EXPECT_EQ(found.blocks_compared, row + 1);
    EXPECT_GT(found.residual, found.tolerance);
    EXPECT_EQ(found.condition_number, std::nullopt);
  }
}

TEST(SparseFaultInjection, AnExponentFlipInTheReportedResidualIsNamedAtItsRow) {
  for (const std::size_t row : {std::size_t{0}, std::size_t{9001}, spec::kRows - 1}) {
    Unit unit = good_unit();
    flip(unit.output, kResidualAt + row * 8, 62);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "residual-row");
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(row));
    EXPECT_EQ(found.blocks_compared, row + 1);
  }
}

TEST(SparseFaultInjection, OfSeveralFailingRowsTheFirstIsNamed) {
  Unit unit = good_unit();
  flip(unit.output, kResidualAt + 12000 * 8, 62);
  flip(unit.output, kResidualAt + 700 * 8, 62);
  flip(unit.output, kResidualAt + 4000 * 8, 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(700));
  EXPECT_EQ(found.blocks_compared, 701U);
}

TEST(SparseFaultInjection, AnIterateThatIsNotASolutionIsCaughtByTheResidualNorm) {
  // x = 0 and r = b is consistent row by row -- every row check holds -- and
  // is nowhere near a solution: only the norm sees it.
  const System system = evidence::generated_system(evidence::kSeed, good_unit().index);
  const std::vector<double> zero(spec::kRows, 0.0);
  const Unit unit{good_unit().index, evidence::input_bytes(system),
                  evidence::output_bytes(zero, system.rhs)};
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "residual-norm");
  EXPECT_EQ(found.element, std::nullopt);
  EXPECT_EQ(found.blocks_compared, spec::kRows + 1);
  double squares = 0.0;
  for (const double b : system.rhs) {
    squares += b * b;
  }
  EXPECT_EQ(found.residual, std::sqrt(squares));
  EXPECT_EQ(found.tolerance,
            spec::residual_norm_factor() * std::sqrt(squares) * (1.0 + spec::kEvaluationSlack));
}

TEST(SparseFaultInjection, ANaNOrInfinityInTheIterateFailsAtTheFirstRowThatReadsIt) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    put_double(unit.output, kIterateAt + 6000 * 8, bad);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(first_row_touching(6000)));
    EXPECT_TRUE(std::isnan(found.residual) || std::isinf(found.residual));
    EXPECT_EQ(found.tolerance, std::numeric_limits<double>::max())
        << "a sum that is not finite is limited to the largest double";
  }
}

TEST(SparseFaultInjection, ANaNOrInfinityInTheReportedResidualIsAMismatch) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    put_double(unit.output, kResidualAt + 42 * 8, bad);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(42));
    EXPECT_TRUE(std::isfinite(found.tolerance));
  }
}

TEST(SparseFaultInjection, AFiniteIterateWhoseProductsOverflowGetsTheLargestTolerance) {
  Unit unit = good_unit();
  put_double(unit.output, kIterateAt, 1.0e308);  // row 0 doubles it: 2e308 overflows
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.tolerance, std::numeric_limits<double>::max());
  EXPECT_TRUE(std::isinf(found.residual));
}

TEST(SparseFaultInjection, ARowWithNothingToSumGetsTheSmallestToleranceSoAnyErrorFails) {
  // A = 2 I, b = e_0, x = e_0 / 2: every residual is exactly zero, and for
  // every row but the first the sum s is zero too, so the tolerance floors.
  const System system =
      evidence::uniform_system(0.0, [](std::size_t i) { return i == 0 ? 1.0 : 0.0; });
  std::vector<double> x(spec::kRows, 0.0);
  x[0] = 0.5;
  std::vector<double> reported(spec::kRows, 0.0);
  const std::vector<std::byte> input = evidence::input_bytes(system);
  EXPECT_TRUE(std::holds_alternative<Agreement>(
      check_unit(Unit{0, input, evidence::output_bytes(x, reported)})));

  reported[7] = 1.0e-300;
  const BoundedMismatch found =
      mismatch_of(check_unit(Unit{0, input, evidence::output_bytes(x, reported)}));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(7));
  EXPECT_EQ(found.tolerance, spec::kMinTolerance);
  EXPECT_EQ(found.residual, 1.0e-300);

  reported[7] = spec::kMinTolerance * 0.5;  // below the floor: held
  EXPECT_TRUE(std::holds_alternative<Agreement>(
      check_unit(Unit{0, input, evidence::output_bytes(x, reported)})));
}

TEST(SparseFaultInjection, AnErrorBelowTheRoundoffOfAProductIsUnseenAndAboveItIsCaught) {
  // Bit 0 of an entry of x moves a product by 2^-52 of itself, inside the
  // rounding the bound allows; bit 40 is far outside it.
  Unit unit = good_unit();
  flip(unit.output, kIterateAt + 5000 * 8, 0);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
  unit = good_unit();
  flip(unit.output, kIterateAt + 5000 * 8, 40);
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
}

TEST(SparseFaultInjection, ACorruptedMatrixValueIsJudgedByTheOutputItDidNotProduce) {
  // The oracle judges the output against the input it is given (oracle.hpp).
  // One value of A moves, the output stays: a mismatch of the output; the
  // runner's bracket says the input was the side that changed. A value changed
  // in only one half of a symmetric pair is not an input problem to the oracle:
  // it does not check symmetry.
  const System system = evidence::generated_system(evidence::kSeed, good_unit().index);
  const std::size_t off_diagonal = system.row_ptr[5000] + 6;  // row 5000's column 6031
  ASSERT_NE(system.columns[off_diagonal], 5000U);
  Unit unit = good_unit();
  flip(unit.input, spec::kValuesOffset + off_diagonal * 8, 40);
  const Verdict verdict = check_unit(unit);
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
}

// T5b ------------------------------------------------------------------------

void expect_not_checked(const Verdict& verdict) {
  ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict))
      << "an unrun check must never be reported as anything else";
  const auto& unchecked = std::get<NotChecked>(verdict);
  EXPECT_EQ(unchecked.blocks_compared, 0U);
  EXPECT_FALSE(unchecked.what.empty());
  EXPECT_FALSE(unchecked.why.empty());
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
}

TEST(SparseNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
  std::vector<Unit> units(8, good_unit());
  units[0].input.pop_back();
  units[1].input.push_back(std::byte{0});
  units[2].input.clear();
  units[3].output.pop_back();
  units[4].output.push_back(std::byte{0});
  units[5].output.clear();
  units[6].input.resize(spec::kInputBytes / 2);
  units[7].output.resize(spec::kOutputBytes * 2);
  for (const Unit& unit : units) {
    expect_not_checked(check_unit(unit));
  }
}

TEST(SparseNotPerformed, ARowPointerThatIsNotTheFixedStructureIsNotChecked) {
  const auto row_ptr_at = [](std::size_t i) { return spec::kRowPtrOffset + i * 4; };
  Unit unit = good_unit();
  put_u32(unit.input, row_ptr_at(0), 1);
  expect_not_checked(check_unit(unit));

  unit = good_unit();
  put_u32(unit.input, row_ptr_at(5),
          load_u32(std::span<const std::byte>(unit.input).subspan(spec::kRowPtrOffset), 5) + 1);
  expect_not_checked(check_unit(unit));

  unit = good_unit();
  put_u32(unit.input, row_ptr_at(5),
          load_u32(std::span<const std::byte>(unit.input).subspan(spec::kRowPtrOffset), 5) - 1);
  expect_not_checked(check_unit(unit));

  unit = good_unit();  // the pointer runs backwards: the row would have a negative length
  put_u32(unit.input, row_ptr_at(spec::kRows), 0);
  expect_not_checked(check_unit(unit));

  unit = good_unit();  // the last pointer is not the number of stored entries
  put_u32(unit.input, row_ptr_at(spec::kRows), static_cast<std::uint32_t>(spec::kNonZeros + 1));
  expect_not_checked(check_unit(unit));
}

TEST(SparseNotPerformed, AColumnThatIsNotTheFixedStructureIsNotChecked) {
  for (const std::size_t entry : {std::size_t{0}, std::size_t{3}, spec::kNonZeros - 1}) {
    Unit unit = good_unit();
    put_u32(unit.input, spec::kColumnsOffset + entry * 4, 12345);
    expect_not_checked(check_unit(unit));
  }
  Unit unit = good_unit();  // a column out of range
  put_u32(unit.input, spec::kColumnsOffset + 40 * 4, 0xFFFFFFFFU);
  expect_not_checked(check_unit(unit));
}

TEST(SparseNotPerformed, AValueTheToleranceDoesNotCoverIsNotChecked) {
  const std::size_t diagonal = spec::kValuesOffset + 0;  // row 0: its first entry is the diagonal
  const std::size_t off_diagonal = spec::kValuesOffset + 8;
  for (const double bad :
       {2.0000000000000004, 1.9999999999999998, 0.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    put_double(unit.input, diagonal, bad);
    expect_not_checked(check_unit(unit));
  }
  for (const double bad :
       {0.12500000000000003, -0.2, 1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    put_double(unit.input, off_diagonal, bad);
    expect_not_checked(check_unit(unit));
  }
  // The magnitude 1/8 itself, either sign, is the largest the tolerance covers.
  for (const double edge : {0.125, -0.125}) {
    Unit unit = good_unit();
    put_double(unit.input, off_diagonal, edge);
    EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(unit)));
  }
}

TEST(SparseNotPerformed, ARightHandSideTheToleranceDoesNotCoverIsNotChecked) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 1.0000000000000002, -1.0000000000000002, 7.0}) {
    for (const std::size_t row : {std::size_t{0}, std::size_t{8000}, spec::kRows - 1}) {
      Unit unit = good_unit();
      put_double(unit.input, spec::kRhsOffset + row * 8, bad);
      expect_not_checked(check_unit(unit));
    }
  }
  Unit edge = good_unit();  // -1 is generated, and is covered
  put_double(edge.input, spec::kRhsOffset + 8 * 8, -1.0);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(edge)));
}

TEST(SparseNotPerformed, AnAllZeroRightHandSideIsNotCheckedEvenWithAnAllZeroOutput) {
  Unit unit = good_unit();
  std::fill(unit.input.begin() + spec::kRhsOffset,
            unit.input.begin() + static_cast<std::ptrdiff_t>(spec::kColumnsOffset), std::byte{0});
  expect_not_checked(check_unit(unit));
  std::fill(unit.output.begin(), unit.output.end(), std::byte{0});
  expect_not_checked(check_unit(unit));
}

TEST(SparseNotPerformed, ANaNInTheOutputIsAMismatchNotAnUnrunCheck) {
  Unit unit = good_unit();
  put_double(unit.output, 0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
}

TEST(SparseNotPerformed, AMatrixThatIsNotSymmetricIsNotAnInputProblemTheOracleChecks) {
  // The mirror of an off-diagonal value changes alone; still within 1/8.
  const System system = evidence::generated_system(evidence::kSeed, good_unit().index);
  Unit unit = good_unit();
  // Row 1's entry at column 0 is the mirror of row 0's entry at column 1.
  const std::size_t mirror = system.row_ptr[1] + 0;
  put_double(unit.input, spec::kValuesOffset + mirror * 8, 0.0625);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(unit)));
}

}  // namespace
}  // namespace loadforge::verification::oracle::sparse
