// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference encoding of control messages and acknowledgements, run
// against the control codec suite, and the definitions src/worker/control.hpp
// carries.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/worker/control_contract.hpp"
#include "worker/control.hpp"

namespace loadforge::contracts {
namespace {

using worker::ControlAck;
using worker::ControlDecodeError;
using worker::ControlDecodeFault;
using worker::ControlMessage;

void put(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>(value >> shift));
  }
}

void put_text(std::vector<std::byte>& out, const std::string& text) {
  put(out, text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(c));
  }
}

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
  bool get(std::uint64_t& out) {
    if (bytes_.size() - at_ < 8) {
      return false;
    }
    out = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      out |= std::to_integer<std::uint64_t>(bytes_[at_ + byte]) << (8 * byte);
    }
    at_ += 8;
    return true;
  }
  template <typename T>
  bool get_as(T& out) {
    std::uint64_t wide = 0;
    const bool ok = get(wide);
    out = static_cast<T>(wide);
    return ok;
  }
  bool text(std::string& out) {
    std::uint64_t size = 0;
    if (!get(size) || bytes_.size() - at_ < size) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes_.data() + at_), size);
    at_ += size;
    return true;
  }
  [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
  std::size_t at_ = 2;
};

std::vector<std::byte> encode_message(const ControlMessage& message) {
  std::vector<std::byte> out{std::byte{worker::kControlFormatVersion},
                             static_cast<std::byte>(message.command.index() + 1)};
  put(out, message.sequence);
  if (const auto* duty = std::get_if<worker::SetDuty>(&message.command)) {
    put(out, duty->busy_permille);
    put(out, static_cast<std::uint64_t>(duty->period_ns));
  } else if (const auto* rebind = std::get_if<worker::Rebind>(&message.command)) {
    put(out, rebind->cpus.size());
    for (const std::uint32_t cpu : rebind->cpus) {
      put(out, cpu);
    }
  } else if (const auto* change = std::get_if<worker::Switch>(&message.command)) {
    put_text(out, change->workload);
    put(out, change->seed);
    put(out, change->footprint_bytes);
    put(out, change->decomposition);
    put(out, change->duty.oracle_period);
    put(out, change->duty.integrity_period);
  }
  return out;
}

core::Result<ControlMessage, ControlDecodeError> decode_message(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return ControlDecodeError{ControlDecodeFault::kEmpty, "no bytes"};
  }
  if (bytes[0] != std::byte{worker::kControlFormatVersion}) {
    return ControlDecodeError{ControlDecodeFault::kVersion, "version"};
  }
  if (bytes.size() < 2) {
    return ControlDecodeError{ControlDecodeFault::kTruncated, "no kind"};
  }
  const auto kind = std::to_integer<unsigned>(bytes[1]);
  if (kind < 1 || kind > 6) {
    return ControlDecodeError{ControlDecodeFault::kKind, std::to_string(kind)};
  }
  Reader in(bytes);
  ControlMessage message;
  bool ok = in.get_as(message.sequence);
  if (ok && kind == 1) {
    message.command = worker::Stop{};
  } else if (ok && kind == 2) {
    message.command = worker::Pause{};
  } else if (ok && kind == 3) {
    message.command = worker::Resume{};
  } else if (ok && kind == 4) {
    worker::SetDuty duty;
    ok = in.get_as(duty.busy_permille) && in.get_as(duty.period_ns);
    message.command = duty;
  } else if (ok && kind == 5) {
    worker::Rebind rebind;
    std::uint64_t count = 0;
    ok = in.get(count) && count <= bytes.size();
    for (std::uint64_t at = 0; ok && at < count; ++at) {
      std::uint32_t cpu = 0;
      ok = in.get_as(cpu);
      rebind.cpus.push_back(cpu);
    }
    message.command = rebind;
  } else if (ok) {
    worker::Switch change;
    ok = in.text(change.workload) && in.get(change.seed) && in.get(change.footprint_bytes) &&
         in.get_as(change.decomposition) && in.get_as(change.duty.oracle_period) &&
         in.get_as(change.duty.integrity_period);
    message.command = change;
  }
  if (!ok) {
    return ControlDecodeError{ControlDecodeFault::kTruncated, "ends inside the message"};
  }
  if (!in.done()) {
    return ControlDecodeError{ControlDecodeFault::kTrailing, "bytes after the message"};
  }
  if (!worker::is_valid(message)) {
    return ControlDecodeError{ControlDecodeFault::kInvalid, "breaks the command's rules"};
  }
  return message;
}

std::vector<std::byte> encode_ack(const ControlAck& ack) {
  std::vector<std::byte> out{std::byte{worker::kControlFormatVersion}, std::byte{0x80}};
  put(out, ack.sequence);
  put(out, static_cast<std::uint64_t>(ack.status));
  put_text(out, ack.detail);
  return out;
}

core::Result<ControlAck, ControlDecodeError> decode_ack(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return ControlDecodeError{ControlDecodeFault::kEmpty, "no bytes"};
  }
  if (bytes[0] != std::byte{worker::kControlFormatVersion}) {
    return ControlDecodeError{ControlDecodeFault::kVersion, "version"};
  }
  if (bytes.size() < 2) {
    return ControlDecodeError{ControlDecodeFault::kTruncated, "no kind"};
  }
  if (bytes[1] != std::byte{0x80}) {
    return ControlDecodeError{ControlDecodeFault::kKind, "not an acknowledgement"};
  }
  Reader in(bytes);
  ControlAck ack;
  if (!(in.get_as(ack.sequence) && in.get_as(ack.status) && in.text(ack.detail))) {
    return ControlDecodeError{ControlDecodeFault::kTruncated, "ends inside the ack"};
  }
  if (!in.done()) {
    return ControlDecodeError{ControlDecodeFault::kTrailing, "bytes after the ack"};
  }
  if (!worker::is_valid(ack)) {
    return ControlDecodeError{ControlDecodeFault::kInvalid, "breaks the ack's rules"};
  }
  return ack;
}

struct ReferenceControlCodec {
  static std::vector<std::byte> encode_message(const ControlMessage& message) {
    return contracts::encode_message(message);
  }
  static core::Result<ControlMessage, ControlDecodeError> decode_message(
      std::span<const std::byte> bytes) {
    return contracts::decode_message(bytes);
  }
  static std::vector<std::byte> encode_ack(const ControlAck& ack) {
    return contracts::encode_ack(ack);
  }
  static core::Result<ControlAck, ControlDecodeError> decode_ack(std::span<const std::byte> bytes) {
    return contracts::decode_ack(bytes);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, ControlCodecContract,
                               ::testing::Types<ReferenceControlCodec>);

TEST(ControlDefinition, CommandNames) {
  EXPECT_EQ(worker::command_name(worker::Stop{}), "stop");
  EXPECT_EQ(worker::command_name(worker::Pause{}), "pause");
  EXPECT_EQ(worker::command_name(worker::Resume{}), "resume");
  EXPECT_EQ(worker::command_name(worker::SetDuty{}), "set duty");
  EXPECT_EQ(worker::command_name(worker::Rebind{}), "rebind");
  EXPECT_EQ(worker::command_name(worker::Switch{}), "switch");
}

TEST(ControlDefinition, ValidityRulesOneAtATime) {
  for (const ControlMessage& message : control_suite::valid_messages()) {
    EXPECT_TRUE(worker::is_valid(message)) << worker::command_name(message.command);
  }
  for (const ControlMessage& message : control_suite::invalid_messages()) {
    EXPECT_FALSE(worker::is_valid(message)) << worker::command_name(message.command);
  }
  EXPECT_TRUE(worker::is_valid(ControlAck{1, worker::AckStatus::kDone, ""}));
  EXPECT_FALSE(worker::is_valid(ControlAck{1, static_cast<worker::AckStatus>(2), ""}));
}

TEST(ControlEquality, MessagesCompareSequenceCommandAndContent) {
  const ControlMessage stop{1, worker::Stop{}};
  EXPECT_EQ(stop, stop);
  EXPECT_FALSE(stop == (ControlMessage{2, worker::Stop{}}));
  EXPECT_FALSE(stop == (ControlMessage{1, worker::Pause{}}));
  EXPECT_EQ((ControlMessage{1, worker::Pause{}}), (ControlMessage{1, worker::Pause{}}));
  EXPECT_EQ((ControlMessage{1, worker::Resume{}}), (ControlMessage{1, worker::Resume{}}));
  EXPECT_FALSE((ControlMessage{1, worker::SetDuty{1, 2}}) ==
               (ControlMessage{1, worker::SetDuty{1, 3}}));
  EXPECT_FALSE((ControlMessage{1, worker::Rebind{{1}}}) ==
               (ControlMessage{1, worker::Rebind{{2}}}));
  EXPECT_FALSE((ControlMessage{1, worker::Switch{"a", 1, 1, 0, {}}}) ==
               (ControlMessage{1, worker::Switch{"b", 1, 1, 0, {}}}));
  EXPECT_TRUE(worker::Stop{} == worker::Stop{});
  expect_memberwise_equality(worker::SetDuty{1, 2},
                             {[](worker::SetDuty& d) { d.busy_permille = 9; },
                              [](worker::SetDuty& d) { d.period_ns = 9; }});
  expect_memberwise_equality(worker::Rebind{{1}}, {[](worker::Rebind& r) { r.cpus.push_back(2); }});
  expect_memberwise_equality(
      worker::Switch{"a", 1, 2, 3, {4, 5}},
      {[](worker::Switch& s) { s.workload = "b"; }, [](worker::Switch& s) { s.seed = 9; },
       [](worker::Switch& s) { s.footprint_bytes = 9; },
       [](worker::Switch& s) { s.decomposition = 9; },
       [](worker::Switch& s) { s.duty.oracle_period = 9; }});
  expect_memberwise_equality(ControlAck{1, worker::AckStatus::kDone, ""},
                             {[](ControlAck& a) { a.sequence = 2; },
                              [](ControlAck& a) { a.status = worker::AckStatus::kRefused; },
                              [](ControlAck& a) { a.detail = "x"; }});
}

}  // namespace
}  // namespace loadforge::contracts
