// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for src/verification/invariant: each check fails under the
// corruption it exists for, and says so with the right kind.
//
//   * sum, squared norm, conserved total: every bit of a claimed result, or of
//     one value after the fact, flipped in turn -- caught whenever it moved
//     the total by more than the bound plus the honest residual, never when
//     it moved it by less than the bound minus it, always for the sign and
//     exponent of a claim;
//   * permutation: every single-bit flip of every key, a dropped key, a
//     duplicated one, a key swapped for another -- all caught;
//   * order: a swap of neighbours, a raised key, a lowered one -- caught at the
//     element where the order breaks.
//
// T5b: a NaN or an infinity among the values leaves no tolerance; the check
// is not made, and the record is VerificationNotPerformed.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/invariant/invariant.hpp"
#include "verification/record.hpp"
#include "verification/residual/residual.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::verification::invariant {
namespace {

std::vector<double> values(std::uint64_t stream, std::size_t n) {
  std::vector<double> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    out[k] = workloads::dense_linear::entry_value(rng::u64_of(rng::block_at(5, stream, k)));
  }
  return out;
}

double flip(double value, int bit) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << bit));
}

/// Every bit of `claim` flipped, judged by `judge`; sharp around the bound.
template <typename Judge>
void every_flip_of_a_claim(double claim, Judge judge, const std::string& what) {
  const residual::Comparison honest = judge(claim).value();
  ASSERT_TRUE(residual::within(honest)) << what;
  for (int bit = 0; bit < 64; ++bit) {
    const double corrupt = flip(claim, bit);
    const residual::Comparison measured = judge(corrupt).value();
    const double moved = std::abs(corrupt - claim);
    if (bit >= 52 || moved > honest.tolerance + honest.residual) {
      EXPECT_FALSE(residual::within(measured)) << what << " bit " << bit;
    } else if (moved < honest.tolerance - honest.residual) {
      EXPECT_TRUE(residual::within(measured)) << what << " bit " << bit;
    }
  }
}

TEST(InvariantCorruption, AClaimedSumWithAFlippedBit) {
  for (std::uint64_t stream = 0; stream < 8; ++stream) {
    const std::vector<double> x = values(stream, 1000);
    double claim = 0.0;
    for (std::size_t k = x.size(); k > 0; --k) {
      claim += x[k - 1];
    }
    every_flip_of_a_claim(claim, [&](double c) { return check_sum(c, x); }, "sum");
  }
}

TEST(InvariantCorruption, AClaimedSquaredNormWithAFlippedBit) {
  for (std::uint64_t stream = 0; stream < 8; ++stream) {
    const std::vector<double> x = values(100 + stream, 1000);
    double claim = 0.0;
    for (std::size_t k = x.size(); k > 0; --k) {
      claim = std::fma(x[k - 1], x[k - 1], claim);
    }
    every_flip_of_a_claim(claim, [&](double c) { return check_squared_norm(c, x); }, "norm");
  }
}

TEST(InvariantCorruption, AConservedTotalWithOneValueCorrupted) {
  // after is a permutation of before (reversed); then one of its values has
  // one bit flipped. The total moves by exactly what the value moved by.
  const std::vector<double> before = values(200, 512);
  const std::vector<double> after(before.rbegin(), before.rend());
  const residual::Comparison honest = check_conserved(before, after).value();
  ASSERT_TRUE(residual::within(honest));
  std::size_t caught = 0;
  for (const std::size_t at : {std::size_t{0}, std::size_t{100}, std::size_t{511}}) {
    for (int bit = 0; bit < 63; ++bit) {
      std::vector<double> corrupt = after;
      corrupt[at] = flip(after[at], bit);
      const Bounded result = check_conserved(before, corrupt);
      if (!result.has_value()) {
        // The top exponent bit makes the value about 2^1023: past what the
        // derivation covers, so not judged -- refused, never passed.
        EXPECT_EQ(result.error(), residual::Underivable::kMagnitudeTooLarge) << bit;
        EXPECT_EQ(bit, 62);
        ++caught;
        continue;
      }
      const residual::Comparison measured = result.value();
      const double moved = std::abs(corrupt[at] - after[at]);
      if (moved > 2.0 * (honest.tolerance + honest.residual)) {
        EXPECT_FALSE(residual::within(measured)) << at << " bit " << bit;
      } else if (moved < (honest.tolerance - honest.residual) / 2.0) {
        EXPECT_TRUE(residual::within(measured)) << at << " bit " << bit;
      }
      caught += residual::within(measured) ? 0 : 1;
    }
  }
  EXPECT_GT(caught, 3U * 30U);
}

TEST(InvariantCorruption, EverySingleBitFlipOfEveryKeyBreaksThePermutation) {
  std::vector<std::uint64_t> keys(256);
  for (std::size_t k = 0; k < keys.size(); ++k) {
    keys[k] = rng::u64_of(rng::block_at(6, 0, k));
  }
  const Fingerprint original = fingerprint(keys);
  for (std::size_t at = 0; at < keys.size(); ++at) {
    for (int bit = 0; bit < 64; ++bit) {
      std::vector<std::uint64_t> corrupt = keys;
      corrupt[at] ^= std::uint64_t{1} << bit;
      const Fingerprint print = fingerprint(corrupt);
      ASSERT_NE(print.low, original.low) << at << " bit " << bit;
      ASSERT_NE(print.high, original.high) << at << " bit " << bit;
      ASSERT_FALSE(is_permutation(keys, corrupt));
    }
  }
}

TEST(InvariantCorruption, ADroppedADuplicatedAndASubstitutedKeyBreakThePermutation) {
  const std::vector<std::uint64_t> keys{9, 2, 7, 7, 1, 0, 42};
  std::vector<std::uint64_t> dropped = keys;
  dropped.pop_back();
  std::vector<std::uint64_t> duplicated = keys;
  duplicated[0] = duplicated[1];  // 9 lost, 2 twice: same count
  std::vector<std::uint64_t> substituted = keys;
  substituted[4] = 3;
  std::vector<std::uint64_t> extra = keys;
  extra.push_back(0);
  for (const std::vector<std::uint64_t>* corrupt : {&dropped, &duplicated, &substituted, &extra}) {
    EXPECT_FALSE(is_permutation(keys, *corrupt));
  }
}

TEST(InvariantCorruption, ABrokenOrderIsFoundWhereItBreaks) {
  std::vector<std::uint64_t> sorted(100);
  for (std::size_t k = 0; k < sorted.size(); ++k) {
    sorted[k] = 10 * k;
  }
  std::vector<std::uint64_t> swapped = sorted;
  std::swap(swapped[40], swapped[41]);
  EXPECT_EQ(first_descent(swapped), 41U);
  std::vector<std::uint64_t> raised = sorted;
  raised[60] |= std::uint64_t{1} << 40;  // one high bit set
  EXPECT_EQ(first_descent(raised), 61U);
  std::vector<std::uint64_t> lowered = sorted;
  lowered[70] = 0;
  EXPECT_EQ(first_descent(lowered), 70U);
  // A flip of the lowest bit of a key between neighbours ten apart keeps
  // the order: the order check is blind to it, and the fingerprint is not.
  std::vector<std::uint64_t> low = sorted;
  low[20] ^= 1U;
  EXPECT_EQ(first_descent(low), std::nullopt);
  EXPECT_FALSE(is_permutation(sorted, low));
}

TEST(InvariantNotPerformed, ValuesWithoutATolerance) {
  std::vector<double> x = values(300, 64);
  x[10] = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> y = values(301, 64);
  y[63] = -std::numeric_limits<double>::infinity();
  const std::vector<Bounded> results{check_sum(0.0, x), check_squared_norm(0.0, x),
                                     check_conserved(x, values(300, 64)),
                                     check_conserved(values(301, 64), y), check_sum(0.0, y)};
  for (const Bounded& result : results) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), residual::Underivable::kNonFiniteMagnitude);
    const ErrorRecord record = VerificationNotPerformed{
        Provenance{}, "invariant", std::string(residual::describe(result.error())), 0};
    EXPECT_EQ(kind_name(record), "VerificationNotPerformed");
    EXPECT_FALSE(is_wrong_result(record));
  }
}

}  // namespace
}  // namespace loadforge::verification::invariant
