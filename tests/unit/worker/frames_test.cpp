// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tally frame: seven counters and a version, cumulative, refused whole when
// it is not exactly that.

#include "worker/frames.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

// M2 contract corrections, 2: the memory frame.
TEST(MemoryFrame, EveryOutcomeSurvivesTheRoundTripAndTheLayoutIsTheDocumentedOne) {
  using verification::LockOutcome;
  using verification::LockPolicy;
  using verification::MemoryLock;
  const MemoryLock locked{LockPolicy::kRequire, LockOutcome::kLocked, {}};
  const MemoryLock none{LockPolicy::kNone, LockOutcome::kNotLocked, {}};
  const MemoryLock fell{LockPolicy::kTry, LockOutcome::kFellBack, "mlock: Cannot allocate memory"};
  for (const MemoryLock& lock : {locked, none, fell}) {
    const auto decoded = decode_memory(encode_memory(lock));
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded.value(), lock);
  }
  const std::vector<std::byte> bytes = encode_memory(fell);
  ASSERT_EQ(bytes.size(), 3U + fell.detail.size());
  EXPECT_EQ(bytes[0], std::byte{1}) << "the version";
  EXPECT_EQ(bytes[1], std::byte{1}) << "kTry";
  EXPECT_EQ(bytes[2], std::byte{2}) << "kFellBack";
  EXPECT_EQ(bytes[3], std::byte{'m'});
}

TEST(MemoryFrame, ALongDetailIsCutOnTheWayOutAndRefusedOnTheWayIn) {
  verification::MemoryLock lock{verification::LockPolicy::kTry,
                                verification::LockOutcome::kFellBack,
                                std::string(kMaxMemoryDetailBytes + 50, 'x')};
  const std::vector<std::byte> bytes = encode_memory(lock);
  EXPECT_EQ(bytes.size(), 3U + kMaxMemoryDetailBytes);
  EXPECT_TRUE(decode_memory(bytes).has_value());
  std::vector<std::byte> longer = bytes;
  longer.push_back(std::byte{'x'});
  const auto refused = decode_memory(longer);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), "memory frame: a detail of 1025 bytes, at most 1024");
}

TEST(MemoryFrame, AnythingThatIsNotAFrameIsRefusedAndSaysWhy) {
  const std::vector<std::byte> good = encode_memory(verification::MemoryLock{});
  for (std::size_t length = 0; length < 3; ++length) {
    const auto refused = decode_memory(std::span(good).first(length));
    EXPECT_EQ(refused.error(),
              "memory frame: " + std::to_string(length) + " byte(s), expected at least 3");
  }
  std::vector<std::byte> bytes = good;
  bytes[0] = std::byte{2};
  EXPECT_EQ(decode_memory(bytes).error(), "memory frame: unknown format version 2");
  bytes = good;
  bytes[1] = std::byte{3};
  EXPECT_EQ(decode_memory(bytes).error(), "memory frame: unknown lock policy 3");
  bytes = good;
  bytes[2] = std::byte{3};
  EXPECT_EQ(decode_memory(bytes).error(), "memory frame: unknown lock outcome 3");
  bytes = good;  // kLocked: a detail has no place.
  bytes.push_back(std::byte{'x'});
  EXPECT_EQ(decode_memory(bytes).error(), "memory frame: a detail on an outcome that has none");
}

}  // namespace
}  // namespace loadforge::worker
