// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_WORKER_CONTROL_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_WORKER_CONTROL_CONTRACT_HPP

// Provider-side suite for the encodings in src/worker/control.hpp
// (ADR 0003, 0004).
//
// ControlCodecContract<Subject>: Subject::encode_message, decode_message,
// encode_ack and decode_ack have the signatures of the worker:: functions.
//
// The behaviour the header describes -- pause, duty, rebind, switch -- is
// tested by the worker-control stream against real worker processes (T7);
// this suite holds the encodings both ends link.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "worker/control.hpp"

namespace loadforge::contracts {

namespace control_suite {

using worker::ControlAck;
using worker::ControlMessage;

inline std::vector<ControlMessage> valid_messages() {
  return {
      {1, worker::Stop{}},
      {2, worker::Pause{}},
      {3, worker::Resume{}},
      {4, worker::SetDuty{1, 1}},
      {5, worker::SetDuty{1000, 60'000'000'000}},
      {6, worker::Rebind{{0}}},
      {7, worker::Rebind{{1, 5, 1023}}},
      {8, worker::Switch{"dense_linear", 42, 1U << 30U, 64, {4, 16}}},
      {~std::uint32_t{0}, worker::Switch{"z", 0, 1, 0, {0, 0}}},
  };
}

inline std::vector<ControlMessage> invalid_messages() {
  return {
      {1, worker::SetDuty{0, 1}},
      {1, worker::SetDuty{1001, 1}},
      {1, worker::SetDuty{500, 0}},
      {1, worker::SetDuty{500, -1}},
      {1, worker::Rebind{{}}},
      {1, worker::Rebind{{3, 2}}},
      {1, worker::Rebind{{2, 2}}},
      {1, worker::Rebind{{1024}}},
      {1, worker::Switch{"Dense", 1, 1, 0, {}}},
      {1, worker::Switch{"", 1, 1, 0, {}}},
      {1, worker::Switch{"dense_linear", 1, 0, 0, {}}},
  };
}

inline std::vector<ControlAck> valid_acks() {
  return {{1, worker::AckStatus::kDone, ""},
          {2, worker::AckStatus::kRefused, "CPU 7 is not in this cpuset"},
          {3, worker::AckStatus::kRefused, std::string(verification::kMaxTextBytes, 'x')}};
}

}  // namespace control_suite

template <typename Subject>
class ControlCodecContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(ControlCodecContract);

using worker::ControlDecodeFault;

TYPED_TEST_P(ControlCodecContract, EveryValidMessageAndAckSurvivesTheRoundTrip) {
  for (const worker::ControlMessage& message : control_suite::valid_messages()) {
    ASSERT_TRUE(worker::is_valid(message)) << worker::command_name(message.command);
    const auto decoded = TypeParam::decode_message(TypeParam::encode_message(message));
    ASSERT_TRUE(decoded.has_value()) << worker::command_name(message.command);
    EXPECT_EQ(decoded.value(), message);
  }
  for (const worker::ControlAck& ack : control_suite::valid_acks()) {
    const auto decoded = TypeParam::decode_ack(TypeParam::encode_ack(ack));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), ack);
  }
}

TYPED_TEST_P(ControlCodecContract, TheFirstTwoBytesAreTheVersionAndTheKind) {
  for (const worker::ControlMessage& message : control_suite::valid_messages()) {
    const std::vector<std::byte> bytes = TypeParam::encode_message(message);
    ASSERT_GE(bytes.size(), 2U);
    EXPECT_EQ(bytes[0], std::byte{worker::kControlFormatVersion});
    EXPECT_EQ(bytes[1], static_cast<std::byte>(message.command.index() + 1));
  }
  const std::vector<std::byte> ack = TypeParam::encode_ack(control_suite::valid_acks()[0]);
  ASSERT_GE(ack.size(), 2U);
  EXPECT_EQ(ack[0], std::byte{worker::kControlFormatVersion});
  EXPECT_EQ(ack[1], std::byte{0x80});
}

TYPED_TEST_P(ControlCodecContract, PrefixesAreTruncatedExtraBytesTrailingAndNothingEmpty) {
  EXPECT_EQ(TypeParam::decode_message({}).error().fault, ControlDecodeFault::kEmpty);
  EXPECT_EQ(TypeParam::decode_ack({}).error().fault, ControlDecodeFault::kEmpty);
  for (const worker::ControlMessage& message : control_suite::valid_messages()) {
    const std::vector<std::byte> bytes = TypeParam::encode_message(message);
    for (std::size_t length = 1; length < bytes.size(); ++length) {
      EXPECT_EQ(TypeParam::decode_message(std::span(bytes).first(length)).error().fault,
                ControlDecodeFault::kTruncated)
          << worker::command_name(message.command) << " prefix " << length;
    }
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    EXPECT_EQ(TypeParam::decode_message(longer).error().fault, ControlDecodeFault::kTrailing);
  }
  const std::vector<std::byte> ack = TypeParam::encode_ack(control_suite::valid_acks()[1]);
  for (std::size_t length = 1; length < ack.size(); ++length) {
    EXPECT_EQ(TypeParam::decode_ack(std::span(ack).first(length)).error().fault,
              ControlDecodeFault::kTruncated);
  }
}

TYPED_TEST_P(ControlCodecContract, AnUnknownVersionOrKindIsRefusedAsSuch) {
  std::vector<std::byte> bytes = TypeParam::encode_message(control_suite::valid_messages()[0]);
  bytes[0] = std::byte{worker::kControlFormatVersion + 1};
  EXPECT_EQ(TypeParam::decode_message(bytes).error().fault, ControlDecodeFault::kVersion);
  bytes = TypeParam::encode_message(control_suite::valid_messages()[0]);
  for (const std::byte kind : {std::byte{0}, std::byte{7}, std::byte{0x80}}) {
    bytes[1] = kind;
    EXPECT_EQ(TypeParam::decode_message(bytes).error().fault, ControlDecodeFault::kKind);
  }
  std::vector<std::byte> ack = TypeParam::encode_ack(control_suite::valid_acks()[0]);
  ack[1] = std::byte{1};
  EXPECT_EQ(TypeParam::decode_ack(ack).error().fault, ControlDecodeFault::kKind)
      << "a message is not an acknowledgement";
}

TYPED_TEST_P(ControlCodecContract, AnInvalidMessageOrAckNeverDecodes) {
  for (const worker::ControlMessage& message : control_suite::invalid_messages()) {
    ASSERT_FALSE(worker::is_valid(message));
    EXPECT_EQ(TypeParam::decode_message(TypeParam::encode_message(message)).error().fault,
              ControlDecodeFault::kInvalid)
        << worker::command_name(message.command);
  }
  for (const worker::ControlAck& ack :
       {worker::ControlAck{1, static_cast<worker::AckStatus>(7), ""},
        worker::ControlAck{1, worker::AckStatus::kRefused,
                           std::string(verification::kMaxTextBytes + 1, 'x')}}) {
    ASSERT_FALSE(worker::is_valid(ack));
    EXPECT_EQ(TypeParam::decode_ack(TypeParam::encode_ack(ack)).error().fault,
              ControlDecodeFault::kInvalid);
  }
}

TYPED_TEST_P(ControlCodecContract, ArbitraryBytesAreAnsweredNeverCrashedOn) {
  for (std::uint64_t stream = 0; stream < 2000; ++stream) {
    std::vector<std::byte> bytes(stream % 61);
    rng::fill_stream(2, stream, bytes);
    if (bytes.size() >= 2 && stream % 2 == 0) {
      bytes[0] = std::byte{worker::kControlFormatVersion};
      bytes[1] = static_cast<std::byte>(1 + stream % 6);
    }
    const auto message = TypeParam::decode_message(bytes);
    if (message.has_value()) {
      EXPECT_TRUE(worker::is_valid(message.value())) << stream;
    }
    const auto ack = TypeParam::decode_ack(bytes);
    if (ack.has_value()) {
      EXPECT_TRUE(worker::is_valid(ack.value())) << stream;
    }
  }
}

REGISTER_TYPED_TEST_SUITE_P(ControlCodecContract, EveryValidMessageAndAckSurvivesTheRoundTrip,
                            TheFirstTwoBytesAreTheVersionAndTheKind,
                            PrefixesAreTruncatedExtraBytesTrailingAndNothingEmpty,
                            AnUnknownVersionOrKindIsRefusedAsSuch,
                            AnInvalidMessageOrAckNeverDecodes,
                            ArbitraryBytesAreAnsweredNeverCrashedOn);

}  // namespace loadforge::contracts

#endif
