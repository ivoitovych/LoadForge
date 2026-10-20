// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bracket's comparison, against record.hpp's rules for a Difference:
// first and last offsets, popcount over the whole input, the window from the
// first differing word, clipped at kMaxWindowBytes and at the end (P1, P3).

#include "verification/integrity/difference.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "verification/record.hpp"

namespace loadforge::verification::integrity {
namespace {

using Bytes = std::vector<std::byte>;

Bytes counting(std::size_t size) {
  Bytes bytes(size);
  for (std::size_t at = 0; at < size; ++at) {
    bytes[at] = static_cast<std::byte>(at * 37 + 11);
  }
  return bytes;
}

TEST(DifferenceTest, EqualBytesHaveNoDifference) {
  const Bytes expected = counting(200);
  EXPECT_FALSE(compare_stored(expected, expected).has_value());
  const Bytes one = counting(1);
  EXPECT_FALSE(compare_stored(one, one).has_value());
}

TEST(DifferenceTest, OneBitAtTheFirstByte) {
  const Bytes expected = counting(200);
  Bytes actual = expected;
  actual[0] ^= std::byte{0x80};
  const auto difference = compare_stored(expected, actual);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->domain, Domain::kStored);
  EXPECT_EQ(difference->first_offset, 0U);
  EXPECT_EQ(difference->last_offset, 0U);
  EXPECT_EQ(difference->popcount, 1U);
  EXPECT_EQ(difference->window_offset, 0U);
  EXPECT_EQ(difference->expected, Bytes(expected.begin(), expected.begin() + 64));
  EXPECT_EQ(difference->actual, Bytes(actual.begin(), actual.begin() + 64));
  EXPECT_TRUE(is_valid(*difference));
  EXPECT_EQ(
      classify_bits(difference->popcount, difference->first_offset, difference->last_offset, 64),
      BitClass::kSingleBit);
}

TEST(DifferenceTest, TheWindowStartsAtTheFirstDifferingWordAndStopsAtTheEnd) {
  const Bytes expected = counting(200);
  Bytes actual = expected;
  actual[163] ^= std::byte{0x03};
  actual[199] ^= std::byte{0x01};
  const auto difference = compare_stored(expected, actual);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->first_offset, 163U);
  EXPECT_EQ(difference->last_offset, 199U);
  EXPECT_EQ(difference->popcount, 3U);
  EXPECT_EQ(difference->window_offset, 160U);
  EXPECT_EQ(difference->expected.size(), 40U) << "from 160 to the end at 200, under 64";
  EXPECT_EQ(difference->actual[3] ^ difference->expected[3], std::byte{0x03});
  EXPECT_EQ(difference->actual[39] ^ difference->expected[39], std::byte{0x01});
  EXPECT_TRUE(is_valid(*difference));
}

TEST(DifferenceTest, ThePopcountCoversBitsOutsideTheWindow) {
  const Bytes expected = counting(1000);
  Bytes actual = expected;
  actual[8] ^= std::byte{0xFF};
  actual[900] ^= std::byte{0x0F};
  const auto difference = compare_stored(expected, actual);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->popcount, 12U) << "8 bits in the window, 4 far past it";
  EXPECT_EQ(difference->window_offset, 8U);
  EXPECT_EQ(difference->expected.size(), kMaxWindowBytes);
  EXPECT_EQ(difference->last_offset, 900U);
  EXPECT_TRUE(is_valid(*difference));
  EXPECT_EQ(
      classify_bits(difference->popcount, difference->first_offset, difference->last_offset, 64),
      BitClass::kAcrossCacheLines);
}

TEST(DifferenceTest, TheLastByteOfAOneByteInput) {
  const Bytes expected{std::byte{0x00}};
  const Bytes actual{std::byte{0xA0}};
  const auto difference = compare_stored(expected, actual);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->popcount, 2U);
  EXPECT_EQ(difference->expected.size(), 1U);
  EXPECT_TRUE(is_valid(*difference));
}

}  // namespace
}  // namespace loadforge::verification::integrity
