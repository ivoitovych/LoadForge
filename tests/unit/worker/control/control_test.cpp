// SPDX-License-Identifier: GPL-3.0-or-later
//
// The encodings of worker/control.hpp: the provider's suite run against them,
// the bytes of the documented layout (made by Python's struct, not by this
// code), and every way a message or an acknowledgement can be refused.

#include "worker/control.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "contracts/worker/control_contract.hpp"

namespace loadforge::worker {
namespace {

struct RealControlCodec {
  static std::vector<std::byte> encode_message(const ControlMessage& message) {
    return encode(message);
  }
  static core::Result<ControlMessage, ControlDecodeError> decode_message(
      std::span<const std::byte> bytes) {
    return worker::decode_message(bytes);
  }
  static std::vector<std::byte> encode_ack(const ControlAck& ack) { return encode(ack); }
  static core::Result<ControlAck, ControlDecodeError> decode_ack(std::span<const std::byte> bytes) {
    return worker::decode_ack(bytes);
  }
};

}  // namespace
}  // namespace loadforge::worker

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Worker, ControlCodecContract,
                               ::testing::Types<worker::RealControlCodec>);
}  // namespace loadforge::contracts

namespace loadforge::worker {
namespace {

std::vector<std::byte> bytes_of_hex(const std::string& hex) {
  std::vector<std::byte> bytes;
  for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
    bytes.push_back(static_cast<std::byte>(std::stoi(hex.substr(at, 2), nullptr, 16)));
  }
  return bytes;
}

// Made by:  python3 -c "import struct; ..."  on the layout control.hpp and
// control.cpp document: version, kind, sequence (u32), then the command's
// fields, all little-endian; text as a u32 length and its bytes; an
// acknowledgement as version, 0x80, sequence, status (u8), text. They are not
// produced by encode(): that would test the encoder against itself.
TEST(ControlBytes, EachCommandIsTheBytesTheLayoutSays) {
  EXPECT_EQ(encode(ControlMessage{7, Stop{}}), bytes_of_hex("010107000000"));
  EXPECT_EQ(encode(ControlMessage{4, SetDuty{250, 2'000'000'000}}),
            bytes_of_hex("010404000000fa000094357700000000"));
  EXPECT_EQ(encode(ControlMessage{5, Rebind{{3, 9}}}),
            bytes_of_hex("010505000000020000000300000009000000"));
  EXPECT_EQ(encode(ControlMessage{9, Switch{"fft", 42, 1U << 20U, 64, {4, 16}}}),
            bytes_of_hex("010609000000030000006666742a000000000000000000100000000000400000000400"
                         "000010000000"));
  EXPECT_EQ(encode(ControlAck{3, AckStatus::kRefused, "no cp"}),
            bytes_of_hex("01800300000001050000006e6f206370"));
}

TEST(ControlBytes, TheBytesDecodeToTheMessagesTheyWereMadeFrom) {
  const auto duty = decode_message(bytes_of_hex("010404000000fa000094357700000000"));
  ASSERT_TRUE(duty.has_value());
  EXPECT_EQ(duty.value(), (ControlMessage{4, SetDuty{250, 2'000'000'000}}));
  const auto ack = decode_ack(bytes_of_hex("01800300000001050000006e6f206370"));
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack.value(), (ControlAck{3, AckStatus::kRefused, "no cp"}));
}

TEST(ControlRefusals, ACountThatPromisesMoreThanTheBytesHoldIsTruncatedAndAllocatesNothing) {
  // A Rebind that claims 2^32 - 1 CPUs in a message of ten bytes.
  const auto claim = decode_message(bytes_of_hex("010501000000ffffffff"));
  ASSERT_FALSE(claim.has_value());
  EXPECT_EQ(claim.error().fault, ControlDecodeFault::kTruncated);
  // A Switch whose name claims 2^32 - 1 bytes.
  const auto name = decode_message(bytes_of_hex("010601000000ffffffff"));
  ASSERT_FALSE(name.has_value());
  EXPECT_EQ(name.error().fault, ControlDecodeFault::kTruncated);
  // An acknowledgement whose detail does.
  const auto detail = decode_ack(bytes_of_hex("01800100000000ffffffff"));
  EXPECT_FALSE(detail.has_value());
}

TEST(ControlRefusals, EveryRefusalSaysWhatWasWrong) {
  EXPECT_EQ(decode_message({}).error().detail, "no bytes");
  EXPECT_EQ(decode_message(bytes_of_hex("09")).error().detail.substr(0, 9), "version 9");
  EXPECT_EQ(decode_message(bytes_of_hex("01")).error().detail, "ends before the kind");
  EXPECT_EQ(decode_message(bytes_of_hex("010701000000")).error().detail, "kind 7");
  EXPECT_EQ(decode_message(bytes_of_hex("0101")).error().detail, "ends inside the message");
  EXPECT_EQ(decode_message(bytes_of_hex("01010100000000")).error().detail,
            "bytes after the message");
  EXPECT_EQ(decode_message(bytes_of_hex("01040100000000")).error().fault,
            ControlDecodeFault::kTruncated);
  EXPECT_EQ(decode_message(bytes_of_hex("01040100000000000000000000000000")).error().detail,
            "a set duty that breaks the rules of its command");
  EXPECT_EQ(decode_ack(bytes_of_hex("0180010000000200000000")).error().detail,
            "an acknowledgement that breaks its rules");
  EXPECT_EQ(decode_ack(bytes_of_hex("0101")).error().detail, "not an acknowledgement");
}

}  // namespace
}  // namespace loadforge::worker
