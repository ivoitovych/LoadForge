// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b. The workload has no verification code of its own: the
// independent oracle checks its output (verification.md §3). What this tier
// owes is the proof that the output is something that check can tell from a
// corrupted one, and says WHICH check fires, and that a check that cannot be
// made reports so (spec.hpp: NotChecked) and never a pass. The check is the
// sparse tests' own reading of the specification (spec_check.hpp).
//
// The cases are the ones verification.md §6 names for a linear solve, and the
// one §4a is about: corruption of the INPUT before the solve redefines the
// problem, the output solves the corrupted system, and the residual check
// passes -- which is why the input-integrity bracket exists. The test asserts
// that limit as a fact, and that the corrupted unit's output differs from the
// clean unit's, so a re-run from the regenerated input would find it.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "oracle/workloads/sparse/spec_check.hpp"
#include "oracle/workloads/sparse/unit_run.hpp"
#include "unit/workloads/sparse/executors.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using check::Outcome;
using check::Verdict;
using tests::Bytes;

struct Solved {
  Bytes input;
  Bytes output;
};

const Solved& clean_unit() {
  static const Solved solved = [] {
    sparse_tests::SerialExecutor serial;
    const auto workload = tests::make_workload(41, 0);
    Solved unit;
    unit.input = tests::generate_input(*workload, 1);
    unit.output = tests::solve(*workload, 1, unit.input, serial);
    return unit;
  }();
  return solved;
}

void flip_bit(Bytes& bytes, std::size_t byte, unsigned bit) {
  bytes[byte] ^= static_cast<std::byte>(1U << bit);
}

void put_f64(Bytes& bytes, std::size_t offset, std::size_t index, double value) {
  std::memcpy(bytes.data() + offset + index * sizeof(double), &value, sizeof(double));
}

void put_u32(Bytes& bytes, std::size_t offset, std::size_t index, std::uint32_t value) {
  std::memcpy(bytes.data() + offset + index * sizeof(std::uint32_t), &value, sizeof(value));
}

TEST(SparseFaultInjection, TheCleanUnitIsAgreedWithEveryCheckMade) {
  const Outcome outcome = check::check(clean_unit().input, clean_unit().output);
  EXPECT_EQ(outcome.verdict, Verdict::kAgreement);
  EXPECT_EQ(outcome.blocks_compared, kRows + 1) << "n row checks and the norm";
}

TEST(SparseFaultInjection, AFlippedBitInTheIterateFiresTheRowCheckOfTheFirstRowThatReadsIt) {
  // x^_j appears in rows j - 1031, j - 97, j - 1, j, j + 1, j + 97, j + 1031
  // that exist; the first check to fail is the least of them. A bit of the
  // exponent, so the change is far above the roundoff of a product.
  for (const std::size_t column :
       {std::size_t{5}, std::size_t{1031}, std::size_t{9000}, kRows - 1}) {
    Bytes output = clean_unit().output;
    flip_bit(output, column * sizeof(double) + 7, 6);  // the top exponent bit
    const Outcome outcome = check::check(clean_unit().input, output);
    ASSERT_EQ(outcome.verdict, Verdict::kBoundedMismatch) << column;
    EXPECT_EQ(outcome.check, "residual-row");
    std::size_t expected_row = 0;
    for (const std::size_t distance : {std::size_t{1031}, std::size_t{97}, std::size_t{1}}) {
      if (column >= distance) {
        expected_row = column - distance;
        break;
      }
    }
    EXPECT_EQ(outcome.element, expected_row) << "x^ index " << column;
    EXPECT_EQ(outcome.blocks_compared, expected_row + 1) << "the 1-based ordinal of the check";
    EXPECT_GT(outcome.residual, outcome.tolerance);
  }
}

TEST(SparseFaultInjection, AFlippedBitInTheExplicitResidualFiresItsOwnRow) {
  for (const std::size_t row : {std::size_t{0}, std::size_t{777}, kRows - 1}) {
    Bytes output = clean_unit().output;
    flip_bit(output, (kRows + row) * sizeof(double) + 7, 6);
    const Outcome outcome = check::check(clean_unit().input, output);
    ASSERT_EQ(outcome.verdict, Verdict::kBoundedMismatch) << row;
    EXPECT_EQ(outcome.check, "residual-row");
    EXPECT_EQ(outcome.element, row);
    EXPECT_EQ(outcome.blocks_compared, row + 1);
  }
}

TEST(SparseFaultInjection, ABitAboveTheRoundoffOfAProductIsSeenAndOneBelowItIsNot) {
  // The tolerance of a row is of order 2 * 8 u * s_i, s_i about 1: a flip of
  // bit 20 of x^_j's mantissa moves it by 2^-32 x, far above; bit 0, by 2^-52 x,
  // below the row's allowance when x is small.
  const double x = check::f64_at(clean_unit().output, 0, 3000);
  ASSERT_LT(std::fabs(x), 1.0);
  Bytes high = clean_unit().output;
  flip_bit(high, 3000 * sizeof(double) + 2, 4);  // mantissa bit 20
  EXPECT_EQ(check::check(clean_unit().input, high).verdict, Verdict::kBoundedMismatch);
  Bytes low = clean_unit().output;
  flip_bit(low, 3000 * sizeof(double), 0);  // mantissa bit 0
  EXPECT_EQ(check::check(clean_unit().input, low).verdict, Verdict::kAgreement)
      << "the least bit of one entry is below a derived tolerance, by design";
}

TEST(SparseFaultInjection, AnIterateNotYetConvergedPassesTheRowChecksAndFailsTheNorm) {
  // x^ scaled by 0.999, with r^ made the exact way r~ is: every row check
  // holds, because the output is self-consistent, and only check n + 1, the
  // residual norm, says it is not a solution.
  Bytes output = clean_unit().output;
  for (std::size_t i = 0; i < kRows; ++i) {
    put_f64(output, 0, i, 0.999 * check::f64_at(clean_unit().output, 0, i));
  }
  std::size_t entry = 0;
  for (std::size_t row = 0; row < kRows; ++row) {
    const std::size_t last = check::u32_at(clean_unit().input, kRowPtrOffset, row + 1);
    double residual = check::f64_at(clean_unit().input, kRhsOffset, row);
    for (; entry < last; ++entry) {
      residual = std::fma(
          -check::f64_at(clean_unit().input, kValuesOffset, entry),
          check::f64_at(output, 0, check::u32_at(clean_unit().input, kColumnsOffset, entry)),
          residual);
    }
    put_f64(output, kRows * sizeof(double), row, residual);
  }
  const Outcome outcome = check::check(clean_unit().input, output);
  ASSERT_EQ(outcome.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(outcome.check, "residual-norm");
  EXPECT_EQ(outcome.blocks_compared, kRows + 1);
  EXPECT_GT(outcome.residual, outcome.tolerance);
}

TEST(SparseFaultInjection, ANaNOrAnInfinityInTheOutputIsAMismatchNeverAPass) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    Bytes in_x = clean_unit().output;
    put_f64(in_x, 0, 40, bad);
    const Outcome x_outcome = check::check(clean_unit().input, in_x);
    EXPECT_EQ(x_outcome.verdict, Verdict::kBoundedMismatch);
    EXPECT_EQ(x_outcome.check, "residual-row");
    EXPECT_EQ(x_outcome.element, 39U) << "the first row that reads it: the left neighbour";
    Bytes in_r = clean_unit().output;
    put_f64(in_r, kRows * sizeof(double), 8000, bad);
    const Outcome r_outcome = check::check(clean_unit().input, in_r);
    EXPECT_EQ(r_outcome.verdict, Verdict::kBoundedMismatch);
    EXPECT_EQ(r_outcome.element, 8000U);
  }
}

TEST(SparseFaultInjection, CorruptionOfTheInputAfterTheSolveIsSeenByTheCheckAgainstThatInput) {
  Bytes input = clean_unit().input;
  flip_bit(input, kValuesOffset + 60 * sizeof(double) + 7, 5);  // an entry's exponent
  const Outcome outcome = check::check(input, clean_unit().output);
  EXPECT_NE(outcome.verdict, Verdict::kAgreement);
}

TEST(SparseFaultInjection, CorruptionOfTheInputBeforeTheSolveIsNotSeenByTheResidualCheck) {
  // verification.md §4a: the corruption redefines the problem. The solver
  // solves A' x = b, the residual of A' is small, and the check -- which has
  // the corrupted matrix it was given -- agrees. It is a limit of the check,
  // not of the workload, and it is what input regeneration is for: the output
  // is not the clean unit's.
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(41, 0);
  Bytes input = clean_unit().input;
  // One off-diagonal value (and not its mirror: entry 1 is row 0's column 1), changed within its
  // range, so the matrix is no longer symmetric but the check does not test that.
  put_f64(input, kValuesOffset, 1, 0.0625);  // row 0's entry at column 1
  const Bytes output = tests::solve(*workload, 1, input, serial);
  const Outcome outcome = check::check(input, output);
  EXPECT_EQ(outcome.verdict, Verdict::kAgreement);
  EXPECT_NE(output, clean_unit().output) << "but different from the clean unit's";
}

// ---- T5b: the check that could not be made --------------------------------

void expect_not_checked(const Bytes& input, const Bytes& output, const char* what) {
  const Outcome outcome = check::check(input, output);
  EXPECT_EQ(outcome.verdict, Verdict::kNotChecked) << what;
  EXPECT_EQ(outcome.blocks_compared, 0U) << what << ": nothing compared is not a clean sweep";
  EXPECT_FALSE(outcome.why.empty()) << what;
}

TEST(SparseFaultInjection, EvidenceOfTheWrongSizeIsNotCheckedNeverPassed) {
  Bytes short_input = clean_unit().input;
  short_input.pop_back();
  expect_not_checked(short_input, clean_unit().output, "short input");
  Bytes long_input = clean_unit().input;
  long_input.push_back(std::byte{0});
  expect_not_checked(long_input, clean_unit().output, "long input");
  Bytes short_output = clean_unit().output;
  short_output.pop_back();
  expect_not_checked(clean_unit().input, short_output, "short output");
  expect_not_checked({}, {}, "no evidence at all");
}

TEST(SparseFaultInjection, AnInputTheToleranceCannotBeDerivedFromIsNotChecked) {
  const auto corrupted = [](auto mutate) {
    Bytes input = clean_unit().input;
    mutate(input);
    return input;
  };
  const Bytes& output = clean_unit().output;
  expect_not_checked(corrupted([](Bytes& in) { put_u32(in, kRowPtrOffset, 10, 3); }), output,
                     "row_ptr differs");
  expect_not_checked(corrupted([](Bytes& in) { put_u32(in, kRowPtrOffset, kRows, 7); }), output,
                     "row_ptr end differs");
  expect_not_checked(corrupted([](Bytes& in) { put_u32(in, kColumnsOffset, 100, 9999); }), output,
                     "a column differs");
  expect_not_checked(corrupted([](Bytes& in) {
                       put_f64(in, kValuesOffset, 1, std::numeric_limits<double>::quiet_NaN());
                     }),
                     output, "a value is NaN");
  expect_not_checked(corrupted([](Bytes& in) {
                       put_f64(in, kValuesOffset, 1, std::numeric_limits<double>::infinity());
                     }),
                     output, "a value is infinite");
  expect_not_checked(corrupted([](Bytes& in) { put_f64(in, kValuesOffset, 1, 0.125000001); }),
                     output, "an off-diagonal value above 1/8");
  expect_not_checked(corrupted([](Bytes& in) { put_f64(in, kValuesOffset, 1, -0.5); }), output,
                     "an off-diagonal value below -1/8");
  expect_not_checked(corrupted([](Bytes& in) {
                       // Entry 0 is row 0's diagonal, the next float above 2.0.
                       put_f64(in, kValuesOffset, 0, 2.0000000000000004);
                     }),
                     output, "a diagonal that is not 2.0");
  expect_not_checked(corrupted([](Bytes& in) {
                       put_f64(in, kRhsOffset, 5, std::numeric_limits<double>::quiet_NaN());
                     }),
                     output, "b is NaN");
  expect_not_checked(corrupted([](Bytes& in) { put_f64(in, kRhsOffset, 5, 1.0000000000000002); }),
                     output, "b above 1");
  expect_not_checked(corrupted([](Bytes& in) {
                       for (std::size_t i = 0; i < kRows; ++i) {
                         put_f64(in, kRhsOffset, i, 0.0);
                       }
                     }),
                     output, "b is all zero");
}

TEST(SparseFaultInjection, AllZeroBytesEvidenceIsNotChecked) {
  expect_not_checked(Bytes(kInputBytes, std::byte{0}), Bytes(kOutputBytes, std::byte{0}), "zeros");
}

}  // namespace
}  // namespace loadforge::workloads::sparse
