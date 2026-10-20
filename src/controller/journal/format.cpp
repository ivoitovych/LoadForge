// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/journal/format.hpp"

#include <algorithm>
#include <array>
#include <bit>

#include "core/crc32.hpp"

namespace loadforge::journal::format {
namespace {

constexpr unsigned kOptionalFields = 6;
constexpr std::uint64_t kReservedBits = ~((std::uint64_t{1} << kOptionalFields) - 1);

void put(std::vector<std::byte>& out, std::uint64_t value, unsigned bytes) {
  for (unsigned at = 0; at < bytes; ++at) {
    out.push_back(static_cast<std::byte>(value >> (8 * at)));
  }
}

std::uint64_t get(std::span<const std::byte> in, std::size_t at, unsigned bytes) {
  std::uint64_t value = 0;
  for (unsigned byte = 0; byte < bytes; ++byte) {
    value |= std::to_integer<std::uint64_t>(in[at + byte]) << (8 * byte);
  }
  return value;
}

std::uint32_t check(std::span<const std::byte> length_bytes, std::span<const std::byte> payload) {
  return core::crc32_update(core::crc32(length_bytes), payload);
}

void put_label(std::vector<std::byte>& out, const std::string& label) {
  put(out, label.size(), 1);
  std::ranges::transform(label, std::back_inserter(out),
                         [](char c) { return static_cast<std::byte>(c); });
}

std::uint64_t bits(std::optional<std::uint64_t> value) { return value.value_or(0); }

std::optional<std::uint64_t> bits(std::optional<double> value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::bit_cast<std::uint64_t>(*value);
}

/// Reads a payload front to back. A read past the end sets `failed_` and
/// answers zero, so a decoder checks once at the end rather than at every
/// field -- and cannot forget to.
class Cursor {
 public:
  explicit Cursor(std::span<const std::byte> bytes) : rest_(bytes) {}

  std::span<const std::byte> take(std::size_t count) {
    if (failed_ || rest_.size() < count) {
      failed_ = true;
      return {};
    }
    const std::span<const std::byte> head = rest_.first(count);
    rest_ = rest_.subspan(count);
    return head;
  }

  std::uint64_t number(unsigned bytes) {
    const std::span<const std::byte> field = take(bytes);
    return failed_ ? 0 : get(field, 0, bytes);
  }

  [[nodiscard]] bool complete() const { return !failed_ && rest_.empty(); }

 private:
  std::span<const std::byte> rest_;
  bool failed_ = false;
};

}  // namespace

std::vector<std::byte> frame(std::span<const std::byte> payload) {
  std::vector<std::byte> out;
  out.reserve(kHeaderBytes + payload.size());
  put(out, payload.size(), 4);
  put(out, check(std::span(out), payload), 4);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::byte> encode_entry(const Entry& entry) {
  std::vector<std::byte> out{kEntry};
  put(out, static_cast<std::uint64_t>(entry.elapsed_ns), 8);
  const std::array<std::optional<std::uint64_t>, kOptionalFields> optionals{
      entry.last_verified_unit,   bits(entry.temperature_c), bits(entry.package_power_w),
      bits(entry.frequency_hz),   entry.ecc_corrected,       entry.ecc_uncorrected};
  std::uint64_t present = 0;
  for (unsigned field = 0; field < kOptionalFields; ++field) {
    present |= std::uint64_t{optionals.at(field).has_value()} << field;
  }
  put(out, present, 1);
  put_label(out, entry.phase);
  put_label(out, entry.workload);
  for (const auto& optional : optionals) {
    put(out, bits(optional), 8);
  }
  put(out, entry.workers_alive, 4);
  put(out, entry.workers_stale, 4);
  return out;
}

std::vector<std::byte> terminator_payload() { return {kTerminator}; }

std::optional<Entry> decode_entry(std::span<const std::byte> payload) {
  Cursor cursor(payload);
  Entry entry;
  static_cast<void>(cursor.take(1));  // the type, which the caller has read
  entry.elapsed_ns = static_cast<std::int64_t>(cursor.number(8));
  const std::uint64_t present = cursor.number(1);
  if ((present & kReservedBits) != 0) {
    return std::nullopt;
  }
  for (std::string* label : {&entry.phase, &entry.workload}) {
    const std::uint64_t size = cursor.number(1);
    if (size > kMaxLabelBytes) {
      return std::nullopt;
    }
    const std::span<const std::byte> text = cursor.take(size);
    label->resize(text.size());
    std::ranges::transform(text, label->begin(),
                           [](std::byte b) { return static_cast<char>(b); });
  }
  std::array<std::optional<std::uint64_t>, kOptionalFields> optionals;
  for (unsigned field = 0; field < kOptionalFields; ++field) {
    const std::uint64_t value = cursor.number(8);
    if (((present >> field) & 1U) != 0) {
      optionals.at(field) = value;
    }
  }
  entry.workers_alive = static_cast<std::uint32_t>(cursor.number(4));
  entry.workers_stale = static_cast<std::uint32_t>(cursor.number(4));
  if (!cursor.complete()) {
    return std::nullopt;
  }
  const auto real = [](std::optional<std::uint64_t> raw) -> std::optional<double> {
    if (!raw.has_value()) {
      return std::nullopt;
    }
    return std::bit_cast<double>(*raw);
  };
  entry.last_verified_unit = optionals[0];
  entry.temperature_c = real(optionals[1]);
  entry.package_power_w = real(optionals[2]);
  entry.frequency_hz = real(optionals[3]);
  entry.ecc_corrected = optionals[4];
  entry.ecc_uncorrected = optionals[5];
  return entry;
}

Replay scan(std::span<const std::byte> bytes) {
  Replay replay;
  if (bytes.empty()) {
    return replay;
  }
  std::size_t at = 0;
  while (true) {
    const std::span<const std::byte> rest = bytes.subspan(at);
    if (rest.empty()) {
      replay.ending = Ending::kUnterminated;
      return replay;
    }
    if (rest.size() < kHeaderBytes) {
      replay.ending = Ending::kTornTail;
      return replay;
    }
    const std::uint64_t length = get(rest, 0, 4);
    if (length == 0 || length > kMaxPayloadBytes) {
      replay.ending = Ending::kCorrupt;
      return replay;
    }
    if (rest.size() - kHeaderBytes < length) {
      replay.ending = Ending::kTornTail;
      return replay;
    }
    const std::span<const std::byte> payload = rest.subspan(kHeaderBytes, length);
    if (check(rest.first(4), payload) != get(rest, 4, 4)) {
      replay.ending = Ending::kCorrupt;
      return replay;
    }
    at += kHeaderBytes + length;
    if (payload[0] == kTerminator) {
      const bool alone = payload.size() == 1 && at == bytes.size();
      replay.ending = alone ? Ending::kTerminated : Ending::kCorrupt;
      return replay;
    }
    std::optional<Entry> entry;
    if (payload[0] == kEntry) {
      entry = decode_entry(payload);
    }
    if (!entry.has_value()) {
      replay.ending = Ending::kCorrupt;
      return replay;
    }
    replay.entries.push_back(std::move(*entry));
  }
}

}  // namespace loadforge::journal::format
