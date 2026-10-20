// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/verification/invariant: the properties each check claims, over
// many generated inputs. A correct result computed in any order is never
// refused -- sums and squared norms in shuffled, reversed, pairwise and
// blocked orders, conserved totals after permutation and exact
// redistribution -- and the premise each bound rests on holds against exact
// arithmetic. A permuted multiset always fingerprints equal; a sorted
// sequence never has a descent.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/verification/residual/exact.hpp"
#include "verification/invariant/invariant.hpp"
#include "verification/residual/residual.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::verification::invariant {
namespace {

constexpr std::uint64_t kSeed = 0xFEEDFACECAFEBEEFULL;

std::uint64_t draw(std::uint64_t stream, std::uint64_t position) {
  return rng::u64_of(rng::block_at(kSeed, stream, position));
}

/// n values, each a multiple of 2^-52 in [-1, 1) times 2^scale.
std::vector<double> values(std::uint64_t stream, std::size_t n, int scale) {
  std::vector<double> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    out[k] = std::ldexp(workloads::dense_linear::entry_value(draw(stream, k)), scale);
  }
  return out;
}

/// Fisher-Yates under Philox: a permutation that is a function of the stream.
template <typename T>
std::vector<T> shuffled(std::vector<T> items, std::uint64_t stream) {
  for (std::size_t i = items.size(); i > 1; --i) {
    std::swap(items[i - 1], items[draw(stream ^ 0x5555, i) % i]);
  }
  return items;
}

double sum_forward(std::span<const double> x) {
  double s = 0.0;
  for (const double v : x) {
    s += v;
  }
  return s;
}

// Pairwise: the range splits at size / 2, left half added to right half.
// Iterative (an explicit stack, left half first), the recursive order exactly.
double sum_pairwise(std::span<const double> x) {
  if (x.empty()) {
    return 0.0;
  }
  struct Step {
    std::size_t first;
    std::size_t size;  // 0: combine the two values on top of the value stack
  };
  std::vector<Step> todo{{0, x.size()}};
  std::vector<double> values;
  while (!todo.empty()) {
    const Step step = todo.back();
    todo.pop_back();
    if (step.size == 0) {
      const double right = values.back();
      values.pop_back();
      values.back() += right;
    } else if (step.size == 1) {
      values.push_back(x[step.first]);
    } else {
      const std::size_t half = step.size / 2;
      todo.push_back({0, 0});
      todo.push_back({step.first + half, step.size - half});
      todo.push_back({step.first, half});
    }
  }
  return values.back();
}

double sum_blocked(std::span<const double> x, std::size_t block) {
  double s = 0.0;
  for (std::size_t p = 0; p < x.size(); p += block) {
    s += sum_forward(x.subspan(p, std::min(block, x.size() - p)));
  }
  return s;
}

/// The sums of `x` another program might claim: the orders above, applied to
/// x and to shuffled copies of it.
std::vector<double> claimed_sums(const std::vector<double>& x, std::uint64_t stream) {
  std::vector<double> reversed(x.rbegin(), x.rend());
  const std::vector<double> mixed = shuffled(x, stream);
  return {sum_forward(x),     sum_forward(reversed), sum_pairwise(x),    sum_pairwise(mixed),
          sum_blocked(x, 16), sum_blocked(mixed, 7), sum_forward(mixed), sum_blocked(reversed, 64)};
}

TEST(InvariantProperty, AnySumOrderIsWithinAndItsErrorWithinGammaAgainstExactArithmetic) {
  double worst_premise = 0.0;
  double worst_ratio = 0.0;
  for (std::uint64_t stream = 0; stream < 200; ++stream) {
    const std::size_t n = 1 + static_cast<std::size_t>(draw(stream, 999) % 2048);
    const std::vector<double> x = values(stream, n, 0);
    exact::Int128 truth = 0;
    exact::Int128 magnitude = 0;
    for (const double v : x) {
      const exact::Int128 count = exact::Int128{exact::scaled52(v)} << 52U;  // units of 2^-104
      truth += count;
      magnitude += count < 0 ? -count : count;
    }
    for (const double claim : claimed_sums(x, stream)) {
      const Bounded result = check_sum(claim, x);
      ASSERT_TRUE(result.has_value());
      EXPECT_TRUE(residual::within(result.value())) << "n " << n;
      worst_ratio = std::max(worst_ratio, result.value().residual / result.value().tolerance);
      const double error = exact::magnitude_of(exact::to_count(claim) - truth);
      const double premise =
          n == 1 ? error : error / (residual::gamma(n - 1) * exact::magnitude_of(magnitude));
      EXPECT_LE(premise, n == 1 ? 0.0 : 1.0) << "n " << n;
      worst_premise = std::max(worst_premise, premise);
    }
  }
  std::cout << "sums: |claim - exact| / (gamma_(n-1) S) at most " << worst_premise
            << "; residual / tolerance at most " << worst_ratio << "\n";
}

TEST(InvariantProperty, AnySquaredNormOrderIsWithin) {
  for (std::uint64_t stream = 0; stream < 200; ++stream) {
    const std::size_t n = 1 + static_cast<std::size_t>(draw(stream, 999) % 1024);
    const std::vector<double> x = values(1000 + stream, n, static_cast<int>(stream % 40) - 20);
    std::vector<double> squares(n);
    std::transform(x.begin(), x.end(), squares.begin(), [](double v) { return v * v; });
    for (const double claim : claimed_sums(squares, stream)) {
      const Bounded result = check_squared_norm(claim, x);
      ASSERT_TRUE(result.has_value());
      EXPECT_TRUE(residual::within(result.value())) << "n " << n;
    }
    // A fused evaluation, as a vector kernel would form it.
    double fused = 0.0;
    for (const double v : shuffled(x, stream)) {
      fused = std::fma(v, v, fused);
    }
    EXPECT_TRUE(residual::within(check_squared_norm(fused, x).value()));
  }
}

TEST(InvariantProperty, ATotalIsConservedUnderPermutationAndExactRedistribution) {
  for (std::uint64_t stream = 0; stream < 200; ++stream) {
    const std::size_t n = 1 + static_cast<std::size_t>(draw(stream, 999) % 1024);
    const std::vector<double> before = values(2000 + stream, n, static_cast<int>(stream % 20));
    EXPECT_TRUE(residual::within(check_conserved(before, shuffled(before, stream)).value()));
    // Every value split exactly in two halves (halving a normal number is
    // exact), then shuffled: the multiset changes, the total does not.
    std::vector<double> after;
    for (const double v : before) {
      after.push_back(v / 2.0);
      after.push_back(v / 2.0);
    }
    EXPECT_TRUE(residual::within(check_conserved(before, shuffled(after, stream + 1)).value()));
    EXPECT_TRUE(residual::within(check_conserved(after, before).value()));
  }
}

TEST(InvariantProperty, APermutationFingerprintsEqualAndASortedSequenceHasNoDescent) {
  for (std::uint64_t stream = 0; stream < 300; ++stream) {
    const auto n = static_cast<std::size_t>(draw(stream, 999) % 600);
    std::vector<std::uint64_t> keys(n);
    for (std::size_t k = 0; k < n; ++k) {
      // A narrow range in some streams, so duplicates are common.
      keys[k] = stream % 3 == 0 ? draw(stream, k) % 8 : draw(stream, k);
    }
    const std::vector<std::uint64_t> mixed = shuffled(keys, stream);
    EXPECT_TRUE(is_permutation(keys, mixed));
    EXPECT_EQ(fingerprint(keys), fingerprint(mixed));
    std::vector<std::uint64_t> sorted = keys;
    std::sort(sorted.begin(), sorted.end());  // the test's sort, not a check's
    EXPECT_EQ(first_descent(sorted), std::nullopt);
    EXPECT_TRUE(is_permutation(keys, sorted));
  }
}

}  // namespace
}  // namespace loadforge::verification::invariant
