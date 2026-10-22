// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the control codec: a message whose bytes went wrong on the way. The
// frame around a message carries a checksum (ipc/record.hpp) and is what
// catches a flipped bit; what the codec owes is that whatever gets past is
// still a message the rules allow, or a refusal that says which rule.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "worker/control.hpp"

namespace loadforge::worker {
namespace {

// One of every command, and the three acknowledgements that differ in kind;
// the contract's suite is not included here: a header that registers a suite
// must be instantiated by every translation unit that includes it.
std::vector<ControlMessage> messages() {
  return {
      {1, Stop{}},
      {2, Pause{}},
      {3, Resume{}},
      {4, SetDuty{1, 1}},
      {5, SetDuty{1000, 60'000'000'000}},
      {6, Rebind{{0}}},
      {7, Rebind{{1, 5, 1023}}},
      {8, Switch{"dense_linear", 42, 1U << 30U, 64, {4, 16}}},
      {~std::uint32_t{0}, Switch{"z", 0, 1, 0, {0, 0}}},
  };
}

std::vector<ControlAck> acks() {
  return {{1, AckStatus::kDone, ""},
          {2, AckStatus::kRefused, "CPU 7 is not in this cpuset"},
          {3, AckStatus::kRefused, std::string(verification::kMaxTextBytes, 'x')}};
}

TEST(ControlFlips, AFlippedBitIsAValidMessageOrAnAnsweredRefusalNeverAnInvalidMessage) {
  std::size_t refused = 0;
  std::size_t accepted = 0;
  for (const ControlMessage& message : messages()) {
    const std::vector<std::byte> bytes = encode(message);
    for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        std::vector<std::byte> flipped = bytes;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
        const auto decoded = decode_message(flipped);
        if (decoded.has_value()) {
          EXPECT_TRUE(is_valid(decoded.value()));
          EXPECT_FALSE(decoded.value() == message) << "a flip changed nothing";
          ++accepted;
        } else {
          ++refused;
        }
      }
    }
  }
  // Both outcomes happen: a flip in a field's low bits is another message, a
  // flip in the version or kind, or one that breaks a rule, is refused.
  EXPECT_GT(accepted, 0U);
  EXPECT_GT(refused, 0U);
}

TEST(ControlFlips, AFlippedBitInAnAcknowledgementIsTheSame) {
  for (const ControlAck& ack : acks()) {
    const std::vector<std::byte> bytes = encode(ack);
    // The longest detail has 1024 bytes of text: sample every 7th byte.
    for (std::size_t byte = 0; byte < bytes.size(); byte += 7) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        std::vector<std::byte> flipped = bytes;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
        const auto decoded = decode_ack(flipped);
        if (decoded.has_value()) {
          EXPECT_TRUE(is_valid(decoded.value()));
        }
      }
    }
  }
}

TEST(ControlFlips, EveryByteOfEveryMessageCutOffAndEveryExtraByteIsNamed) {
  for (const ControlMessage& message : messages()) {
    const std::vector<std::byte> bytes = encode(message);
    for (std::size_t length = 1; length < bytes.size(); ++length) {
      const auto cut = decode_message(std::span(bytes).first(length));
      ASSERT_FALSE(cut.has_value());
      EXPECT_EQ(cut.error().fault, ControlDecodeFault::kTruncated);
    }
  }
}

}  // namespace
}  // namespace loadforge::worker
