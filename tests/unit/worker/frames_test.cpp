// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tally frame: seven counters and a version, cumulative, refused whole when
// it is not exactly that.

#include "worker/frames.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace loadforge::worker {
namespace {

verification::Tally a_tally() {
  verification::Tally tally;
  tally.units_computed = 1;
  tally.oracle_checked = 2;
  tally.oracle_blocks = 0x0102030405060708ULL;
  tally.oracle_failed = 4;
  tally.oracle_not_performed = 5;
  tally.integrity_checked = 6;
  tally.integrity_failed = 0xFFFFFFFFFFFFFFFFULL;
  return tally;
}

TEST(TallyFrame, EveryCounterSurvivesTheRoundTrip) {
  const std::vector<std::byte> bytes = encode_tally(a_tally());
  EXPECT_EQ(bytes.size(), kTallyFrameBytes);
  auto decoded = decode_tally(bytes);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded.value(), a_tally());
  EXPECT_EQ(decode_tally(encode_tally(verification::Tally{})).value(), verification::Tally{});
}

TEST(TallyFrame, TheLayoutIsTheVersionThenLittleEndianCountersInTheTallysOrder) {
  const std::vector<std::byte> bytes = encode_tally(a_tally());
  EXPECT_EQ(bytes[0], std::byte{1});
  EXPECT_EQ(bytes[1], std::byte{1}) << "units_computed, low byte first";
  EXPECT_EQ(bytes[1 + 8], std::byte{2}) << "oracle_checked";
  EXPECT_EQ(bytes[1 + 16], std::byte{0x08}) << "oracle_blocks, low byte first";
  EXPECT_EQ(bytes[1 + 16 + 7], std::byte{0x01});
  EXPECT_EQ(bytes[1 + 48], std::byte{0xFF}) << "integrity_failed, the last";
}

TEST(TallyFrame, AnythingButExactlyOneFrameIsRefusedAndSaysWhy) {
  const std::vector<std::byte> bytes = encode_tally(a_tally());
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    auto decoded = decode_tally(std::span(bytes).first(length));
    ASSERT_FALSE(decoded.has_value()) << length;
    EXPECT_NE(decoded.error().find("byte(s), expected 57"), std::string::npos) << decoded.error();
  }
  std::vector<std::byte> longer = bytes;
  longer.push_back(std::byte{0});
  EXPECT_FALSE(decode_tally(longer).has_value());
  std::vector<std::byte> other = bytes;
  other[0] = std::byte{2};
  auto decoded = decode_tally(other);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), "tally frame: unknown format version 2");
}

}  // namespace
}  // namespace loadforge::worker
