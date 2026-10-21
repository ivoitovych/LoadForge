// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for the dense-linear workload: metamorphic properties of C = A B that
// hold EXACTLY in IEEE 754 arithmetic for this workload's order of operations,
// on every path and at every depth -- so each is a bit comparison, not a
// tolerance:
//
//   * A = I gives C = B, and B = I gives C = A: every product but one is a
//     signed zero, and adding a zero to anything changes nothing but the sign
//     of a zero sum.
//   * (B^T A^T) = (A B)^T: a_ik b_kj and b_kj a_ik are the same product, taken
//     in the same order of k, so the transposed problem gives the transposed
//     bits.
//   * Permuting the rows of A permutes the rows of C, and permuting the
//     columns of B permutes its columns: entries never mix.
//   * Scaling A by a power of two scales C by it, and negating A negates C:
//     both commute with every rounding (no overflow or underflow here).
//
// And one that holds only within the bound: permuting k (A's columns and B's
// rows alike) reorders every sum, which changes the bits and must not take
// any entry outside the derived tolerance.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

using simd::IsaPath;

constexpr std::uint32_t kDepths[] = {16, 64, 256};

/// The workload's output for `input` on `path` at `depth`.
Bytes product(IsaPath path, std::uint32_t depth, const Bytes& input) {
  const auto workload = make(config_for(path, depth));
  Serial serial;
  return computed(*workload, 0, input, serial);
}

Bytes generated_input(std::uint64_t index) {
  return generated(*make(config_for(IsaPath::kScalar, 0, 41)), index);
}

/// Every runnable path at every depth in kDepths, with a label.
template <typename Body>
void for_each_configuration(Body body) {
  for (const IsaPath path : runnable_paths()) {
    for (const std::uint32_t depth : kDepths) {
      SCOPED_TRACE(std::string(simd::describe(path)) + " at depth " + std::to_string(depth));
      body(path, depth);
    }
  }
}

TEST(DenseLinearProperty, TheIdentityOnEitherSideGivesTheOtherFactorExactly) {
  const Bytes input = generated_input(0);
  const Bytes identity_left =
      input_of([](std::size_t i, std::size_t k) { return i == k ? 1.0 : 0.0; },
               [&](std::size_t k, std::size_t j) { return b_of(input, k, j); });
  const Bytes identity_right =
      input_of([&](std::size_t i, std::size_t k) { return a_of(input, i, k); },
               [](std::size_t k, std::size_t j) { return k == j ? 1.0 : 0.0; });
  for_each_configuration([&](IsaPath path, std::uint32_t depth) {
    const Bytes left = product(path, depth, identity_left);
    const Bytes right = product(path, depth, identity_right);
    for (std::size_t i = 0; i < kN; ++i) {
      for (std::size_t j = 0; j < kN; ++j) {
        ASSERT_EQ(bits_of(c_of(left, i, j)), bits_of(b_of(input, i, j)))
            << "I B, " << i << ", " << j;
        ASSERT_EQ(bits_of(c_of(right, i, j)), bits_of(a_of(input, i, j)))
            << "A I, " << i << ", " << j;
      }
    }
  });
}

TEST(DenseLinearProperty, TheTransposedProblemGivesTheTransposedBits) {
  const Bytes input = generated_input(1);
  const Bytes transposed =
      input_of([&](std::size_t i, std::size_t k) { return b_of(input, k, i); },
               [&](std::size_t k, std::size_t j) { return a_of(input, j, k); });
  for_each_configuration([&](IsaPath path, std::uint32_t depth) {
    const Bytes c = product(path, depth, input);
    const Bytes ct = product(path, depth, transposed);
    for (std::size_t i = 0; i < kN; ++i) {
      for (std::size_t j = 0; j < kN; ++j) {
        ASSERT_EQ(bits_of(c_of(ct, j, i)), bits_of(c_of(c, i, j))) << i << ", " << j;
      }
    }
  });
}

TEST(DenseLinearProperty, PermutedRowsOfAAndColumnsOfBPermuteCExactly) {
  const Bytes input = generated_input(2);
  const std::vector<std::size_t> rows = Shuffled::permutation(kN, 7);
  const std::vector<std::size_t> columns = Shuffled::permutation(kN, 8);
  const Bytes permuted =
      input_of([&](std::size_t i, std::size_t k) { return a_of(input, rows[i], k); },
               [&](std::size_t k, std::size_t j) { return b_of(input, k, columns[j]); });
  for_each_configuration([&](IsaPath path, std::uint32_t depth) {
    const Bytes c = product(path, depth, input);
    const Bytes cp = product(path, depth, permuted);
    for (std::size_t i = 0; i < kN; ++i) {
      for (std::size_t j = 0; j < kN; ++j) {
        ASSERT_EQ(bits_of(c_of(cp, i, j)), bits_of(c_of(c, rows[i], columns[j]))) << i << ", " << j;
      }
    }
  });
}

TEST(DenseLinearProperty, PowersOfTwoAndNegationCommuteWithTheProduct) {
  const Bytes input = generated_input(3);
  std::vector<std::pair<double, Bytes>> scaled;
  for (const double factor : {2.0, 0.125, -1.0, -4.0}) {
    scaled.emplace_back(
        factor, input_of([&](std::size_t i, std::size_t k) { return factor * a_of(input, i, k); },
                         [&](std::size_t k, std::size_t j) { return b_of(input, k, j); }));
  }
  for_each_configuration([&](IsaPath path, std::uint32_t depth) {
    const Bytes c = product(path, depth, input);
    for (const auto& [factor, scaled_input] : scaled) {
      const Bytes cs = product(path, depth, scaled_input);
      for (std::size_t e = 0; e < kMatrix; ++e) {
        // Values, not bits: a zero sum is +0.0 whatever the sign of its terms.
        ASSERT_EQ(get(cs, e), factor * get(c, e)) << "factor " << factor << ", entry " << e;
      }
    }
  });
}

TEST(DenseLinearProperty, PermutingKReordersEverySumAndStaysWithinTheBound) {
  const Bytes input = generated_input(4);
  const std::vector<std::size_t> order = Shuffled::permutation(kN, 9);
  const Bytes reordered =
      input_of([&](std::size_t i, std::size_t k) { return a_of(input, i, order[k]); },
               [&](std::size_t k, std::size_t j) { return b_of(input, order[k], j); });
  for (const IsaPath path : runnable_paths()) {
    const Bytes c = product(path, kDefaultPanelDepth, input);
    const Bytes cr = product(path, kDefaultPanelDepth, reordered);
    EXPECT_NE(c, cr) << simd::describe(path) << ": a different order, different bits";
    for (std::size_t i = 0; i < kN; i += 5) {
      for (std::size_t j = 0; j < kN; j += 3) {
        const Reference r = reference(input, i, j);
        ASSERT_TRUE(within_bound(c_of(cr, i, j), c_of(c, i, j), r.s))
            << simd::describe(path) << ": c(" << i << ", " << j << ")";
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
