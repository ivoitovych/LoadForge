// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/control.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"

namespace loadforge::worker {
namespace {

constexpr std::byte kAckKind{0x80};
constexpr std::size_t kHeaderBytes = 2;
constexpr std::size_t kCpuBytes = 4;

/// Little-endian, whatever the machine: the controller and its workers are one
/// binary, but a message is kept as bytes by the journal of a bug report and is
/// read on any architecture.
// A value and the width it is written in: the two are told apart by their names, and a struct
// for the pair would be one more type for a function of three lines.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void put(std::vector<std::byte>& out, std::uint64_t value, std::size_t width) {
  for (std::size_t byte = 0; byte < width; ++byte) {
    out.push_back(static_cast<std::byte>(value >> (8 * byte)));
  }
}

void put_text(std::vector<std::byte>& out, const std::string& text) {
  put(out, text.size(), 4);
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(c));
  }
}

std::vector<std::byte> header(std::byte kind) { return {std::byte{kControlFormatVersion}, kind}; }

/// Reads fields in order from after the two header bytes. A read past the end
/// fails once and for all: every later read answers zero, so a decoder asks
/// for all of its fields and looks at ok() once, and no field is believed
/// after the first that was missing.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes.subspan(kHeaderBytes)) {}

  std::uint64_t get(std::size_t width) {
    if (!ok_ || bytes_.size() - at_ < width) {
      ok_ = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < width; ++byte) {
      value |= std::to_integer<std::uint64_t>(bytes_[at_ + byte]) << (8 * byte);
    }
    at_ += width;
    return value;
  }

  std::string text() {
    const std::uint64_t size = get(4);
    if (!ok_ || bytes_.size() - at_ < size) {
      ok_ = false;
      return {};
    }
    std::string out;
    out.reserve(size);
    for (std::uint64_t index = 0; index < size; ++index) {
      out.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[at_ + index])));
    }
    at_ += size;
    return out;
  }

  /// How many `width`-byte items the bytes still hold, for a count that must
  /// not be believed before it is checked (a corrupt count must not allocate).
  [[nodiscard]] std::size_t room_for(std::size_t width) const {
    return (bytes_.size() - at_) / width;
  }

  void fail() { ok_ = false; }
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

ControlDecodeError refusal(ControlDecodeFault fault, std::string detail) {
  ControlDecodeError error;
  error.fault = fault;
  error.detail = std::move(detail);
  return error;
}

/// The checks both decoders share, in the order a reader can make them: bytes
/// at all, the version before anything else is believed, and a kind byte.
std::optional<ControlDecodeError> check_header(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return refusal(ControlDecodeFault::kEmpty, "no bytes");
  }
  if (bytes[0] != std::byte{kControlFormatVersion}) {
    return refusal(ControlDecodeFault::kVersion,
                   "version " + std::to_string(std::to_integer<unsigned>(bytes[0])) +
                       ", this build reads " + std::to_string(kControlFormatVersion));
  }
  if (bytes.size() < kHeaderBytes) {
    return refusal(ControlDecodeFault::kTruncated, "ends before the kind");
  }
  return std::nullopt;
}

/// What is left to say once the fields are read: they were all there, nothing
/// follows them, and (for a message) they keep the rules of their command.
std::optional<ControlDecodeError> check_end(const Reader& in) {
  if (!in.ok()) {
    return refusal(ControlDecodeFault::kTruncated, "ends inside the message");
  }
  if (!in.done()) {
    return refusal(ControlDecodeFault::kTrailing, "bytes after the message");
  }
  return std::nullopt;
}

Command read_command(unsigned kind, Reader& in) {
  switch (kind) {
    case 1:
      return Stop{};
    case 2:
      return Pause{};
    case 3:
      return Resume{};
    case 4: {
      SetDuty duty;
      duty.busy_permille = static_cast<std::uint16_t>(in.get(2));
      duty.period_ns = static_cast<std::int64_t>(in.get(8));
      return duty;
    }
    case 5: {
      Rebind rebind;
      const std::uint64_t count = in.get(4);
      if (count > in.room_for(kCpuBytes)) {
        in.fail();
        return rebind;
      }
      for (std::uint64_t at = 0; at < count; ++at) {
        rebind.cpus.push_back(static_cast<std::uint32_t>(in.get(kCpuBytes)));
      }
      return rebind;
    }
    default: {
      Switch change;
      change.workload = in.text();
      change.seed = in.get(8);
      change.footprint_bytes = in.get(8);
      change.decomposition = static_cast<std::uint32_t>(in.get(4));
      change.duty.oracle_period = static_cast<std::uint32_t>(in.get(4));
      change.duty.integrity_period = static_cast<std::uint32_t>(in.get(4));
      return change;
    }
  }
}

}  // namespace

std::vector<std::byte> encode(const ControlMessage& message) {
  std::vector<std::byte> out = header(static_cast<std::byte>(message.command.index() + 1));
  put(out, message.sequence, 4);
  if (const auto* duty = std::get_if<SetDuty>(&message.command)) {
    put(out, duty->busy_permille, 2);
    put(out, static_cast<std::uint64_t>(duty->period_ns), 8);
  } else if (const auto* rebind = std::get_if<Rebind>(&message.command)) {
    put(out, rebind->cpus.size(), 4);
    for (const std::uint32_t cpu : rebind->cpus) {
      put(out, cpu, kCpuBytes);
    }
  } else if (const auto* change = std::get_if<Switch>(&message.command)) {
    put_text(out, change->workload);
    put(out, change->seed, 8);
    put(out, change->footprint_bytes, 8);
    put(out, change->decomposition, 4);
    put(out, change->duty.oracle_period, 4);
    put(out, change->duty.integrity_period, 4);
  }
  return out;
}

core::Result<ControlMessage, ControlDecodeError> decode_message(std::span<const std::byte> bytes) {
  if (auto refused = check_header(bytes); refused.has_value()) {
    return *refused;
  }
  const auto kind = std::to_integer<unsigned>(bytes[1]);
  if (kind < 1 || kind > std::variant_size_v<Command>) {
    return refusal(ControlDecodeFault::kKind, "kind " + std::to_string(kind));
  }
  Reader in(bytes);
  ControlMessage message;
  message.sequence = static_cast<std::uint32_t>(in.get(4));
  message.command = read_command(kind, in);
  if (auto refused = check_end(in); refused.has_value()) {
    return *refused;
  }
  if (!is_valid(message)) {
    return refusal(ControlDecodeFault::kInvalid, std::string{"a "} +
                                                     std::string{command_name(message.command)} +
                                                     " that breaks the rules of its command");
  }
  return message;
}

std::vector<std::byte> encode(const ControlAck& ack) {
  std::vector<std::byte> out = header(kAckKind);
  put(out, ack.sequence, 4);
  put(out, static_cast<std::uint64_t>(ack.status), 1);
  put_text(out, ack.detail);
  return out;
}

core::Result<ControlAck, ControlDecodeError> decode_ack(std::span<const std::byte> bytes) {
  if (auto refused = check_header(bytes); refused.has_value()) {
    return *refused;
  }
  if (bytes[1] != kAckKind) {
    return refusal(ControlDecodeFault::kKind, "not an acknowledgement");
  }
  Reader in(bytes);
  ControlAck ack;
  ack.sequence = static_cast<std::uint32_t>(in.get(4));
  ack.status = static_cast<AckStatus>(in.get(1));
  ack.detail = in.text();
  if (auto refused = check_end(in); refused.has_value()) {
    return *refused;
  }
  if (!is_valid(ack)) {
    return refusal(ControlDecodeFault::kInvalid, "an acknowledgement that breaks its rules");
  }
  return ack;
}

}  // namespace loadforge::worker
