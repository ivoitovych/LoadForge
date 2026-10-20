// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for exact comparison: over random buffers and random sets of flipped
// bits, every field of the Difference agrees with what the flipped bits ARE,
// computed here from the bit positions rather than from bytes -- a second
// derivation, so the two can disagree.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/checksum/compare.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

/// The classification from the definitions in record.hpp's words: one bit; all
/// bits in one aligned word; all in one aligned line; otherwise across lines.
BitClass by_definition(const std::set<std::uint64_t>& bits, std::uint64_t line_bytes) {
  if (bits.size() == 1) {
    return BitClass::kSingleBit;
  }
  const std::uint64_t first = *bits.begin() / 8;
  const std::uint64_t last = *bits.rbegin() / 8;
  if (first / kWordBytes == last / kWordBytes) {
    return BitClass::kMultiBitInWord;
  }
  if (first / line_bytes == last / line_bytes) {
    return BitClass::kWithinCacheLine;
  }
  return BitClass::kAcrossCacheLines;
}

TEST(CompareProperty, TheDifferenceIsWhatTheFlippedBitsAre) {
  std::mt19937_64 random(20261020);
  for (int round = 0; round < 3000; ++round) {
    const std::size_t size = 1 + random() % 600;
    Bytes expected(size);
    rng::fill_stream(77, static_cast<std::uint64_t>(round), expected);
    // Flips clustered sometimes and spread sometimes, so every class occurs.
    const std::uint64_t span_bits = (round % 4 == 0) ? 64 : (round % 4 == 1) ? 512 : size * 8;
    const std::uint64_t base = random() % (size * 8);
    std::set<std::uint64_t> bits;
    const int count = 1 + static_cast<int>(random() % 6);
    for (int flip = 0; flip < count; ++flip) {
      bits.insert(std::min<std::uint64_t>(size * 8 - 1, base + random() % span_bits));
    }
    Bytes actual = expected;
    for (const std::uint64_t bit : bits) {
      actual[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    }
    const std::uint64_t line_bytes = (round % 3 == 0) ? 128 : 64;
    const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
    ASSERT_EQ(comparison.outcome, ComparisonOutcome::kMismatch) << round;
    const Difference& difference = *comparison.difference;
    EXPECT_TRUE(is_valid(difference)) << round;
    EXPECT_EQ(difference.popcount, bits.size()) << round;
    EXPECT_EQ(difference.first_offset, *bits.begin() / 8) << round;
    EXPECT_EQ(difference.last_offset, *bits.rbegin() / 8) << round;
    EXPECT_EQ(difference.window_offset % kWordBytes, 0U);
    EXPECT_LE(difference.window_offset, difference.first_offset);
    EXPECT_GT(difference.window_offset + kWordBytes, difference.first_offset);
    const std::size_t window =
        std::min<std::size_t>(kMaxWindowBytes, size - difference.window_offset);
    ASSERT_EQ(difference.expected.size(), window) << round;
    // The window's XOR holds exactly the flipped bits that fall inside it.
    const std::vector<std::byte> delta = xor_delta(difference);
    std::uint64_t in_window = 0;
    for (std::size_t at = 0; at < delta.size(); ++at) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        const bool set = (std::to_integer<unsigned>(delta[at]) >> bit & 1U) != 0;
        const std::uint64_t position = (difference.window_offset + at) * 8 + bit;
        EXPECT_EQ(set, bits.contains(position)) << round << " bit " << position;
        in_window += set ? 1 : 0;
      }
    }
    EXPECT_LE(in_window, difference.popcount);
    EXPECT_EQ(classify_bits(difference.popcount, difference.first_offset, difference.last_offset,
                            line_bytes),
              by_definition(bits, line_bytes))
        << round;
  }
}

TEST(CompareProperty, ComparisonIsSymmetricInEverythingButTheBytesShown) {
  std::mt19937_64 random(7);
  for (int round = 0; round < 500; ++round) {
    const std::size_t size = 1 + random() % 300;
    Bytes left(size);
    Bytes right(size);
    rng::fill_stream(1, static_cast<std::uint64_t>(round), left);
    right = left;
    right[random() % size] ^= std::byte{0x42};
    right[random() % size] ^= std::byte{0x81};
    const Comparison forward = compare_exact(left, right, Domain::kStored);
    const Comparison backward = compare_exact(right, left, Domain::kStored);
    ASSERT_EQ(forward.outcome, backward.outcome);
    if (forward.outcome != ComparisonOutcome::kMismatch) {
      continue;  // the two flips cancelled
    }
    EXPECT_EQ(forward.difference->popcount, backward.difference->popcount);
    EXPECT_EQ(forward.difference->first_offset, backward.difference->first_offset);
    EXPECT_EQ(forward.difference->last_offset, backward.difference->last_offset);
    EXPECT_EQ(forward.difference->expected, backward.difference->actual);
    EXPECT_EQ(xor_delta(*forward.difference), xor_delta(*backward.difference));
  }
}

TEST(CompareProperty, AnyRecordMadeFromAComparisonIsValidAndDecodesTheSameKind) {
  std::mt19937_64 random(11);
  for (int round = 0; round < 500; ++round) {
    const std::size_t size = random() % 100;
    Bytes expected(size);
    rng::fill_stream(2, static_cast<std::uint64_t>(round), expected);
    Bytes actual = expected;
    if (round % 3 == 1 && size > 0) {
      actual[random() % size] ^= std::byte{0x08};
    }
    if (round % 3 == 2) {
      actual.push_back(std::byte{0});
    }
    Subject subject;
    subject.check = "property";
    subject.line_bytes = 1U << (3 + round % 6);
    const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
    const std::optional<ErrorRecord> record = to_record(comparison, subject);
    EXPECT_EQ(record.has_value(), comparison.outcome != ComparisonOutcome::kMatch);
    if (record.has_value()) {
      EXPECT_TRUE(is_valid(*record)) << round;
      EXPECT_EQ(is_wrong_result(*record),
                comparison.outcome != ComparisonOutcome::kNothingCompared);
    }
  }
}

}  // namespace
}  // namespace loadforge::verification
