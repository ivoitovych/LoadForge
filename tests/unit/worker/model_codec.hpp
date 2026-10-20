// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKER_MODEL_CODEC_HPP
#define LOADFORGE_TESTS_UNIT_WORKER_MODEL_CODEC_HPP

// The worker's own reading of the record encoding of verification/record.hpp:
// every integer eight bytes little-endian, every optional a flag byte and then
// its value, every string and byte vector a length and its bytes. A different
// layout from the provider's on purpose -- the contract fixes only the first
// two bytes -- so that what the two share is what the contract says.
//
// Strict, as the contract asks: whatever is not exactly one encoded record is
// refused as the fault it is, in byte order (version, kind, then the fields,
// then trailing bytes), and a record the contract forbids is kInvalid.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "verification/record.hpp"

namespace loadforge::worker_testing {

namespace codec_detail {

class Writer {
 public:
  void u8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
  }
  void text(const std::string& value) {
    u64(value.size());
    for (const char c : value) {
      u8(static_cast<std::uint8_t>(c));
    }
  }
  void raw(const std::vector<std::byte>& value) {
    u64(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  template <typename T, typename Put>
  void optional(const std::optional<T>& value, Put put) {
    u8(value.has_value() ? 1 : 0);
    if (value.has_value()) {
      put(*value);
    }
  }
  std::vector<std::byte> bytes;
};

struct Truncated {};
struct Malformed {
  std::string what;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
  std::uint8_t u8() {
    need(1);
    return std::to_integer<std::uint8_t>(bytes_[at_++]);
  }
  std::uint64_t u64() {
    need(8);
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value |= std::to_integer<std::uint64_t>(bytes_[at_++]) << shift;
    }
    return value;
  }
  std::string text() {
    const std::uint64_t length = u64();
    need(length);
    std::string value;
    for (std::uint64_t index = 0; index < length; ++index) {
      value.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[at_++])));
    }
    return value;
  }
  std::vector<std::byte> raw() {
    const std::uint64_t length = u64();
    need(length);
    std::vector<std::byte> value(bytes_.begin() + static_cast<std::ptrdiff_t>(at_),
                                 bytes_.begin() + static_cast<std::ptrdiff_t>(at_ + length));
    at_ += length;
    return value;
  }
  template <typename T, typename Get>
  std::optional<T> optional(Get get) {
    const std::uint8_t flag = u8();
    if (flag > 1) {
      throw Malformed{"a presence flag of " + std::to_string(flag)};
    }
    if (flag == 0) {
      return std::nullopt;
    }
    return get();
  }
  [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

 private:
  void need(std::uint64_t count) const {
    if (count > bytes_.size() - at_) {
      throw Truncated{};
    }
  }
  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
};

inline void put(Writer& out, const verification::Provenance& p) {
  const auto u32 = [&](std::uint32_t v) { out.u64(v); };
  const auto u64 = [&](std::uint64_t v) { out.u64(v); };
  out.optional(p.logical_cpu, u32);
  out.optional(p.physical_core, u32);
  out.optional(p.numa_node, u32);
  out.optional(p.buffer_address, u64);
  out.optional(p.buffer_offset, u64);
  out.optional(p.iteration, u64);
  out.optional(p.workload, [&](const std::string& v) { out.text(v); });
  out.optional(p.workload_version, u32);
  out.optional(p.seed, u64);
  out.optional(p.isa, [&](simd::IsaPath v) { out.u8(static_cast<std::uint8_t>(v)); });
  out.optional(p.input_integrity,
               [&](verification::InputIntegrity v) { out.u8(static_cast<std::uint8_t>(v)); });
  out.optional(p.duty, [&](const verification::DutyCycle& v) {
    out.u64(v.oracle_period);
    out.u64(v.integrity_period);
  });
}

inline verification::Provenance get_provenance(Reader& in) {
  verification::Provenance p;
  const auto u32 = [&] { return static_cast<std::uint32_t>(in.u64()); };
  const auto u64 = [&] { return in.u64(); };
  p.logical_cpu = in.optional<std::uint32_t>(u32);
  p.physical_core = in.optional<std::uint32_t>(u32);
  p.numa_node = in.optional<std::uint32_t>(u32);
  p.buffer_address = in.optional<std::uint64_t>(u64);
  p.buffer_offset = in.optional<std::uint64_t>(u64);
  p.iteration = in.optional<std::uint64_t>(u64);
  p.workload = in.optional<std::string>([&] { return in.text(); });
  p.workload_version = in.optional<std::uint32_t>(u32);
  p.seed = in.optional<std::uint64_t>(u64);
  p.isa = in.optional<simd::IsaPath>([&] { return static_cast<simd::IsaPath>(in.u8()); });
  p.input_integrity = in.optional<verification::InputIntegrity>(
      [&] { return static_cast<verification::InputIntegrity>(in.u8()); });
  p.duty = in.optional<verification::DutyCycle>([&] {
    verification::DutyCycle duty;
    duty.oracle_period = static_cast<std::uint32_t>(in.u64());
    duty.integrity_period = static_cast<std::uint32_t>(in.u64());
    return duty;
  });
  return p;
}

}  // namespace codec_detail

struct ModelCodec {
  static std::vector<std::byte> encode(const verification::ErrorRecord& record) {
    using namespace codec_detail;
    Writer out;
    out.u8(verification::kRecordFormatVersion);
    out.u8(static_cast<std::uint8_t>(record.index() + 1));
    put(out, verification::provenance_of(record));
    if (const auto* exact = std::get_if<verification::ExactBitCorruption>(&record)) {
      out.u8(static_cast<std::uint8_t>(exact->side));
      out.text(exact->check);
      out.u64(exact->line_bytes);
      out.optional(exact->difference, [&](const verification::Difference& d) {
        out.u8(static_cast<std::uint8_t>(d.domain));
        out.u64(d.first_offset);
        out.u64(d.last_offset);
        out.u64(d.popcount);
        out.u64(d.window_offset);
        out.raw(d.expected);
        out.raw(d.actual);
      });
    } else if (const auto* numerical =
                   std::get_if<verification::NumericalVerificationFailure>(&record)) {
      out.text(numerical->check);
      out.u64(std::bit_cast<std::uint64_t>(numerical->residual));
      out.u64(std::bit_cast<std::uint64_t>(numerical->tolerance));
      out.optional(numerical->condition_number,
                   [&](double v) { out.u64(std::bit_cast<std::uint64_t>(v)); });
      out.optional(numerical->element, [&](std::uint64_t v) { out.u64(v); });
    } else {
      const auto& unrun = std::get<verification::VerificationNotPerformed>(record);
      out.text(unrun.what);
      out.text(unrun.why);
      out.u64(unrun.blocks_compared);
    }
    return out.bytes;
  }

  static core::Result<verification::ErrorRecord, verification::DecodeError> decode(
      std::span<const std::byte> bytes) {
    using namespace codec_detail;
    using verification::DecodeFault;
    if (bytes.empty()) {
      return verification::DecodeError{DecodeFault::kEmpty, "no bytes"};
    }
    if (bytes[0] != std::byte{verification::kRecordFormatVersion}) {
      return verification::DecodeError{DecodeFault::kVersion, "version byte"};
    }
    if (bytes.size() < 2) {
      return verification::DecodeError{DecodeFault::kTruncated, "no kind byte"};
    }
    const auto kind = std::to_integer<unsigned>(bytes[1]);
    if (kind < 1 || kind > 3) {
      return verification::DecodeError{DecodeFault::kKind, "kind byte " + std::to_string(kind)};
    }
    try {
      Reader in(bytes.subspan(2));
      verification::ErrorRecord record;
      const verification::Provenance where = get_provenance(in);
      if (kind == 1) {
        verification::ExactBitCorruption exact;
        exact.provenance = where;
        exact.side = static_cast<verification::Side>(in.u8());
        exact.check = in.text();
        exact.line_bytes = static_cast<std::uint32_t>(in.u64());
        exact.difference = in.optional<verification::Difference>([&] {
          verification::Difference d;
          d.domain = static_cast<verification::Domain>(in.u8());
          d.first_offset = in.u64();
          d.last_offset = in.u64();
          d.popcount = in.u64();
          d.window_offset = in.u64();
          d.expected = in.raw();
          d.actual = in.raw();
          return d;
        });
        record = std::move(exact);
      } else if (kind == 2) {
        verification::NumericalVerificationFailure numerical;
        numerical.provenance = where;
        numerical.check = in.text();
        numerical.residual = std::bit_cast<double>(in.u64());
        numerical.tolerance = std::bit_cast<double>(in.u64());
        numerical.condition_number =
            in.optional<double>([&] { return std::bit_cast<double>(in.u64()); });
        numerical.element = in.optional<std::uint64_t>([&] { return in.u64(); });
        record = std::move(numerical);
      } else {
        verification::VerificationNotPerformed unrun;
        unrun.provenance = where;
        unrun.what = in.text();
        unrun.why = in.text();
        unrun.blocks_compared = in.u64();
        record = std::move(unrun);
      }
      if (!in.done()) {
        return verification::DecodeError{DecodeFault::kTrailing, "bytes after the record"};
      }
      if (!verification::is_valid(record)) {
        return verification::DecodeError{DecodeFault::kInvalid, "a record the contract forbids"};
      }
      return record;
    } catch (const Truncated&) {
      return verification::DecodeError{DecodeFault::kTruncated, "ends inside the record"};
    } catch (const Malformed& malformed) {
      return verification::DecodeError{DecodeFault::kInvalid, malformed.what};
    }
  }
};

}  // namespace loadforge::worker_testing

#endif
