// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the dense-linear workload: corruption injected where memory would
// corrupt it, and what reaches the output. The workload performs no
// verification (the oracle stream's does); what it owes the doctrine
// (docs/verification.md §4a, §6) is that it neither hides a fault nor spreads
// one beyond where the arithmetic carries it, so that the bounded check sees
// the fault and the error record can name where it is. Each test injects one
// fault and asserts exactly which entries it reaches and whether the derived
// bound sees it:
//
//   * a flipped exponent bit in a_ik reaches row i of C and nothing else, and
//     breaches the bound against the regenerated input -- while the same
//     output checked against the CORRUPTED input passes: the corruption has
//     redefined the problem, which is why the input bracket exists (§4a);
//   * a flipped bit in b_kj reaches column j and nothing else;
//   * a whole cache line of B zeroed reaches its eight columns and no other;
//   * a NaN in A makes its whole row NaN, which the bound never admits;
//   * a flipped bit in C after compute breaches the bound at that entry
//     alone when it is high, and is invisible when it is the lowest mantissa
//     bit: below the tolerance, a bounded contract cannot see a flip, and an
//     exact check of the input is what catches the faults it misses.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

using simd::IsaPath;

/// Entries of C whose bits differ between two outputs.
std::vector<std::size_t> differing(const Bytes& lhs, const Bytes& rhs) {
  std::vector<std::size_t> entries;
  for (std::size_t e = 0; e < kMatrix; ++e) {
    if (bits_of(get(lhs, e)) != bits_of(get(rhs, e))) {
      entries.push_back(e);
    }
  }
  return entries;
}

/// How many of row i's entries breach the bound against `input`.
std::size_t breaches_in_row(const Bytes& output, const Bytes& input, std::size_t i) {
  std::size_t breaches = 0;
  for (std::size_t j = 0; j < kN; ++j) {
    const Reference r = reference(input, i, j);
    breaches += within_bound(c_of(output, i, j), r.c, r.s) ? 0 : 1;
  }
  return breaches;
}

struct Unit {
  std::unique_ptr<Workload> workload;
  Bytes input;
  Bytes output;
};

Unit clean_unit(IsaPath path) {
  Unit unit{make(config_for(path, 32, 51)), {}, {}};
  unit.input = generated(*unit.workload, 6);
  Serial serial;
  unit.output = computed(*unit.workload, 6, unit.input, serial);
  return unit;
}

Bytes recomputed(const Unit& unit, const Bytes& input) {
  Shuffled executor(4, 3);
  return computed(*unit.workload, 6, input, executor);
}

TEST(DenseLinearFault, AFlippedBitInAReachesItsRowAloneAndTheBoundSeesIt) {
  constexpr std::size_t kI = 77;
  constexpr std::size_t kK = 130;
  for (const IsaPath path : runnable_paths()) {
    const Unit unit = clean_unit(path);
    Bytes corrupt = unit.input;
    flip(corrupt, kI * kN + kK, 57);  // exponent + 32: a_ik times 2^32
    const Bytes output = recomputed(unit, corrupt);

    const std::vector<std::size_t> changed = differing(output, unit.output);
    ASSERT_FALSE(changed.empty()) << simd::describe(path);
    for (const std::size_t e : changed) {
      EXPECT_EQ(e / kN, kI) << simd::describe(path) << ": entry " << e << " is outside row " << kI;
    }
    EXPECT_EQ(breaches_in_row(output, unit.input, kI), kN)
        << simd::describe(path) << ": every entry of the row, against the regenerated input";
    EXPECT_EQ(breaches_in_row(output, corrupt, kI), 0U)
        << simd::describe(path) << ": against the corrupted input itself, nothing is wrong -- "
        << "the corruption redefined the problem";
  }
}

TEST(DenseLinearFault, AFlippedBitInBReachesItsColumnAlone) {
  constexpr std::size_t kK = 3;
  constexpr std::size_t kJ = 250;
  for (const IsaPath path : runnable_paths()) {
    const Unit unit = clean_unit(path);
    Bytes corrupt = unit.input;
    flip(corrupt, kMatrix + kK * kN + kJ, 61);
    const Bytes output = recomputed(unit, corrupt);
    const std::vector<std::size_t> changed = differing(output, unit.output);
    ASSERT_FALSE(changed.empty()) << simd::describe(path);
    for (const std::size_t e : changed) {
      EXPECT_EQ(e % kN, kJ) << simd::describe(path) << ": entry " << e;
    }
  }
}

TEST(DenseLinearFault, AZeroedCacheLineOfBReachesItsEightColumnsAlone) {
  constexpr std::size_t kK = 200;
  constexpr std::size_t kFirst = 64;  // 64 doubles in: a cache line of its own
  for (const IsaPath path : runnable_paths()) {
    const Unit unit = clean_unit(path);
    Bytes corrupt = unit.input;
    for (std::size_t j = kFirst; j < kFirst + 8; ++j) {
      put(corrupt, kMatrix + kK * kN + j, 0.0);
    }
    const Bytes output = recomputed(unit, corrupt);
    const std::vector<std::size_t> changed = differing(output, unit.output);
    ASSERT_FALSE(changed.empty()) << simd::describe(path);
    for (const std::size_t e : changed) {
      EXPECT_GE(e % kN, kFirst) << simd::describe(path);
      EXPECT_LT(e % kN, kFirst + 8) << simd::describe(path);
    }
  }
}

TEST(DenseLinearFault, ANaNInAMakesItsWholeRowNaNAndNoOther) {
  constexpr std::size_t kI = 255;
  for (const IsaPath path : runnable_paths()) {
    const Unit unit = clean_unit(path);
    Bytes corrupt = unit.input;
    put(corrupt, kI * kN + 0, std::numeric_limits<double>::quiet_NaN());
    const Bytes output = recomputed(unit, corrupt);
    for (std::size_t i = 0; i < kN; ++i) {
      for (std::size_t j = 0; j < kN; ++j) {
        ASSERT_EQ(std::isnan(c_of(output, i, j)), i == kI)
            << simd::describe(path) << ": c(" << i << ", " << j << ")";
      }
    }
    EXPECT_EQ(breaches_in_row(output, unit.input, kI), kN) << "a NaN is never within the bound";
  }
}

TEST(DenseLinearFault, AFlippedBitInCIsSeenAboveTheToleranceAndOnlyThere) {
  constexpr std::size_t kI = 10;
  constexpr std::size_t kJ = 20;
  for (const IsaPath path : runnable_paths()) {
    const Unit unit = clean_unit(path);
    const Reference r = reference(unit.input, kI, kJ);
    ASSERT_TRUE(within_bound(c_of(unit.output, kI, kJ), r.c, r.s));

    for (const unsigned bit : {51U, 40U, 62U}) {
      Bytes corrupt = unit.output;
      flip(corrupt, kI * kN + kJ, bit);
      EXPECT_FALSE(within_bound(c_of(corrupt, kI, kJ), r.c, r.s))
          << simd::describe(path) << ": bit " << bit;
      EXPECT_EQ(differing(corrupt, unit.output), std::vector<std::size_t>{kI * kN + kJ});
    }
    Bytes lowest = unit.output;
    flip(lowest, kI * kN + kJ, 0);
    EXPECT_TRUE(within_bound(c_of(lowest, kI, kJ), r.c, r.s))
        << simd::describe(path) << ": one unit in the last place is below any tolerance a "
        << "bounded check can derive here, so this flip is invisible to it";
  }
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
