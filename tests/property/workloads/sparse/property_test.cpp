// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the solver that hold for every unit, and metamorphic
// relations that need no oracle.
//
//   * Scaling b by a power of two scales every vector of the recurrence by it
//     exactly (alpha and beta are ratios of quantities that scale together),
//     so x^ and r^ scale exactly.
//   * Negating b negates every vector: round-to-nearest is symmetric.
//   * A unit's output is a function of its input alone: not of the units
//     computed before it, nor of what the scratch held.
//   * The specification's check holds for every seed.
//   * Compute is total: random garbage in every part of the input is computed
//     on, deterministically, within the work of a valid unit.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "oracle/workloads/sparse/spec_check.hpp"
#include "oracle/workloads/sparse/unit_run.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/sparse/executors.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using tests::Bytes;

/// The input with every b_i replaced by `transform(b_i)`.
template <typename Transform>
Bytes with_rhs(Bytes input, Transform transform) {
  for (std::size_t i = 0; i < kRows; ++i) {
    const double b = check::f64_at(input, kRhsOffset, i);
    const double changed = transform(b);
    std::memcpy(input.data() + kRhsOffset + i * sizeof(double), &changed, sizeof(double));
  }
  return input;
}

double x_at(const Bytes& output, std::size_t i) { return check::f64_at(output, 0, i); }
double r_at(const Bytes& output, std::size_t i) {
  return check::f64_at(output, kRows * sizeof(double), i);
}

TEST(SparseProperty, ScalingTheRightHandSideByAPowerOfTwoScalesTheOutputExactly) {
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(31, 0);
  ASSERT_NE(workload, nullptr);
  const Bytes input = tests::generate_input(*workload, 2);
  const Bytes base = tests::solve(*workload, 2, input, serial);
  for (const int exponent : {-40, -5, 1, 7, 60}) {
    const Bytes scaled_input =
        with_rhs(input, [exponent](double b) { return std::ldexp(b, exponent); });
    const Bytes scaled = tests::solve(*workload, 2, scaled_input, serial);
    for (std::size_t i = 0; i < kRows; ++i) {
      ASSERT_EQ(x_at(scaled, i), std::ldexp(x_at(base, i), exponent)) << "x " << i << " 2^" << exponent;
      ASSERT_EQ(r_at(scaled, i), std::ldexp(r_at(base, i), exponent)) << "r " << i << " 2^" << exponent;
    }
  }
}

TEST(SparseProperty, NegatingTheRightHandSideNegatesTheOutput) {
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(32, 512);
  ASSERT_NE(workload, nullptr);
  const Bytes input = tests::generate_input(*workload, 4);
  const Bytes base = tests::solve(*workload, 4, input, serial);
  const Bytes negated = tests::solve(
      *workload, 4, with_rhs(input, [](double b) { return -b; }), serial);
  for (std::size_t i = 0; i < kRows; ++i) {
    ASSERT_EQ(x_at(negated, i), -x_at(base, i)) << i;
    ASSERT_EQ(r_at(negated, i), -r_at(base, i)) << i;
  }
}

TEST(SparseProperty, AUnitsOutputDoesNotDependOnWhatWasComputedBeforeItOrLeftInScratch) {
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(33, 0);
  ASSERT_NE(workload, nullptr);
  const Bytes first = tests::generate_input(*workload, 0);
  const Bytes second = tests::generate_input(*workload, 1);
  const Bytes alone = tests::solve(*workload, 1, second, serial);

  Bytes scratch(workload->plan().scratch_bytes, std::byte{0});
  Bytes output(workload->plan().output_bytes);
  workload->compute(0, first, output, scratch, serial);
  EXPECT_NE(output, alone);
  workload->compute(1, second, output, scratch, serial);
  EXPECT_EQ(output, alone) << "after another unit, in its scratch";
  workload->compute(1, second, output, scratch, serial);
  EXPECT_EQ(output, alone) << "again, over its own scratch";
}

TEST(SparseProperty, TheSpecificationsCheckHoldsForEverySeedAndUnit) {
  sparse_tests::SerialExecutor serial;
  for (std::uint64_t seed = 100; seed < 112; ++seed) {
    const auto workload = tests::make_workload(seed, 0);
    ASSERT_NE(workload, nullptr);
    const std::uint64_t index = seed % 5;
    const Bytes input = tests::generate_input(*workload, index);
    const Bytes output = tests::solve(*workload, index, input, serial);
    const check::Outcome outcome = check::check(input, output);
    EXPECT_EQ(outcome.verdict, check::Verdict::kAgreement)
        << "seed " << seed << ": " << outcome.check << " " << outcome.element;
  }
}

/// `input` with `count` random bytes of one part replaced by noise.
Bytes garbled(Bytes input, std::size_t offset, std::size_t size, std::uint64_t seed) {
  Bytes noise(size);
  rng::fill_stream(seed, 0xF00D, noise);
  std::ranges::copy(noise, input.begin() + static_cast<std::ptrdiff_t>(offset));
  return input;
}

TEST(SparseProperty, GarbageInAnyPartOfTheInputIsComputedOnDeterministicallyAndWithoutEscaping) {
  sparse_tests::SerialExecutor serial;
  sparse_tests::PoolExecutor pool(3);
  const auto workload = tests::make_workload(34, 256);
  ASSERT_NE(workload, nullptr);
  const Bytes input = tests::generate_input(*workload, 0);
  struct Part {
    std::size_t offset;
    std::size_t size;
    const char* name;
  };
  const Part parts[] = {
      {kValuesOffset, kNonZeros * sizeof(double), "values"},
      {kRhsOffset, kRows * sizeof(double), "rhs"},
      {kColumnsOffset, kNonZeros * sizeof(std::uint32_t), "columns"},
      {kRowPtrOffset, (kRows + 1) * sizeof(std::uint32_t), "row_ptr"},
      {0, kInputBytes, "everything"},
  };
  std::uint64_t seed = 1;
  for (const Part& part : parts) {
    const Bytes corrupt = garbled(input, part.offset, part.size, seed++);
    const Bytes once = tests::solve(*workload, 0, corrupt, serial);
    EXPECT_EQ(once.size(), kOutputBytes) << part.name;
    EXPECT_EQ(tests::solve(*workload, 0, corrupt, pool), once) << part.name << ": any thread count";
    EXPECT_EQ(tests::solve(*workload, 0, corrupt, serial, std::byte{0xFF}), once)
        << part.name << ": whatever the scratch held";
  }
}

TEST(SparseProperty, ACorruptRowPointerCostsNoMoreThanAValidUnit) {
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(35, 2048);
  ASSERT_NE(workload, nullptr);
  Bytes corrupt = tests::generate_input(*workload, 0);
  // Every row claims the whole matrix: row_ptr = 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, ...
  for (std::size_t i = 0; i <= kRows; ++i) {
    const std::uint32_t word = i % 2 == 0 ? 0U : 0xFFFFFFFFU;
    std::memcpy(corrupt.data() + kRowPtrOffset + i * sizeof(std::uint32_t), &word, sizeof(word));
  }
  const Bytes output = tests::solve(*workload, 0, corrupt, serial);
  EXPECT_EQ(output.size(), kOutputBytes);
  // A row is at most seven entries whatever row_ptr says, so the solve ends
  // in the time of a valid unit; here it ends at all, as a function of its input.
  EXPECT_EQ(tests::solve(*workload, 0, corrupt, serial), output);
}

}  // namespace
}  // namespace loadforge::workloads::sparse
