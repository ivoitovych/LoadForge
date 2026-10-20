// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/record/codec.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "verification/record/format.hpp"

namespace loadforge::verification {

namespace {

constexpr std::uint8_t kExactKind = 1;
constexpr std::uint8_t kNumericalKind = 2;
constexpr std::uint8_t kNotPerformedKind = 3;
constexpr std::size_t kHeaderBytes = 2;

// --------------------------------------------------------------------------
// Writing. Every field fixed-width and little-endian (codec.hpp).
// --------------------------------------------------------------------------

class Writer {
 public:
  template <std::size_t Width>
  void unsigned_le(std::uint64_t value) {
    for (std::size_t index = 0; index < Width; ++index) {
      bytes_.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
    }
  }
  void u8(std::uint8_t value) { unsigned_le<1>(value); }
  void u32(std::uint32_t value) { unsigned_le<4>(value); }
  void u64(std::uint64_t value) { unsigned_le<8>(value); }
  void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void raw(std::span<const std::byte> data) {
    u64(data.size());
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  void text(std::string_view value) { raw(std::as_bytes(std::span(value))); }
  void presence(bool present) { u8(present ? 1 : 0); }

  std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

void put_optional_u32(Writer& writer, const std::optional<std::uint32_t>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.u32(*value);
  }
}

void put_optional_u64(Writer& writer, const std::optional<std::uint64_t>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.u64(*value);
  }
}

void put_provenance(Writer& writer, const Provenance& provenance) {
  put_optional_u32(writer, provenance.logical_cpu);
  put_optional_u32(writer, provenance.physical_core);
  put_optional_u32(writer, provenance.numa_node);
  put_optional_u64(writer, provenance.buffer_address);
  put_optional_u64(writer, provenance.buffer_offset);
  put_optional_u64(writer, provenance.iteration);
  writer.presence(provenance.workload.has_value());
  if (provenance.workload.has_value()) {
    writer.text(*provenance.workload);
  }
  put_optional_u32(writer, provenance.workload_version);
  put_optional_u64(writer, provenance.seed);
  writer.presence(provenance.isa.has_value());
  if (provenance.isa.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*provenance.isa));
  }
  writer.presence(provenance.input_integrity.has_value());
  if (provenance.input_integrity.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*provenance.input_integrity));
  }
  writer.presence(provenance.duty.has_value());
  if (provenance.duty.has_value()) {
    writer.u32(provenance.duty->oracle_period);
    writer.u32(provenance.duty->integrity_period);
  }
}

void put_kind(Writer& writer, const ExactBitCorruption& record) {
  writer.u8(static_cast<std::uint8_t>(record.side));
  writer.text(record.check);
  writer.u32(record.line_bytes);
  writer.presence(record.difference.has_value());
  if (record.difference.has_value()) {
    const Difference& difference = *record.difference;
    writer.u8(static_cast<std::uint8_t>(difference.domain));
    writer.u64(difference.first_offset);
    writer.u64(difference.last_offset);
    writer.u64(difference.popcount);
    writer.u64(difference.window_offset);
    writer.raw(difference.expected);
    writer.raw(difference.actual);
  }
}

void put_kind(Writer& writer, const NumericalVerificationFailure& record) {
  writer.text(record.check);
  writer.f64(record.residual);
  writer.f64(record.tolerance);
  writer.presence(record.condition_number.has_value());
  if (record.condition_number.has_value()) {
    writer.f64(*record.condition_number);
  }
  put_optional_u64(writer, record.element);
}

void put_kind(Writer& writer, const VerificationNotPerformed& record) {
  writer.text(record.what);
  writer.text(record.why);
  writer.u64(record.blocks_compared);
}

// --------------------------------------------------------------------------
// Reading. Each call either reads its field or records why it could not and
// returns false; the first failure is the answer (codec.hpp, "How decode()
// answers").
// --------------------------------------------------------------------------

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes), at_(kHeaderBytes) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }
  [[nodiscard]] std::size_t at() const noexcept { return at_; }
  [[nodiscard]] const DecodeError& error() const noexcept { return error_; }

  bool unsigned_le(std::string_view field, std::size_t width, std::uint64_t& out) {
    if (!need(field, width)) {
      return false;
    }
    out = 0;
    for (std::size_t index = 0; index < width; ++index) {
      out |= std::to_integer<std::uint64_t>(bytes_[at_ + index]) << (8U * index);
    }
    at_ += width;
    return true;
  }

  bool u8(std::string_view field, std::uint8_t& out) {
    std::uint64_t wide = 0;
    const bool read = unsigned_le(field, 1, wide);
    out = static_cast<std::uint8_t>(wide);
    return read;
  }

  bool u32(std::string_view field, std::uint32_t& out) {
    std::uint64_t wide = 0;
    const bool read = unsigned_le(field, 4, wide);
    out = static_cast<std::uint32_t>(wide);
    return read;
  }

  bool u64(std::string_view field, std::uint64_t& out) { return unsigned_le(field, 8, out); }

  bool f64(std::string_view field, double& out) {
    std::uint64_t bits = 0;
    const bool read = unsigned_le(field, 8, bits);
    out = std::bit_cast<double>(bits);
    return read;
  }

  /// A length, refused as kInvalid above `limit` before it is trusted for
  /// anything, then that many bytes.
  bool raw(std::string_view field, std::string_view what, std::size_t limit,
           std::span<const std::byte>& out) {
    const std::size_t field_at = at_;
    std::uint64_t length = 0;
    if (!u64(field, length)) {
      return false;
    }
    if (length > limit) {
      return fail(DecodeFault::kInvalid, where(field, field_at) + ": " + std::string(what) +
                                             " of " + std::to_string(length) +
                                             " bytes, more than the " + std::to_string(limit) +
                                             " a record carries");
    }
    if (!need(field, length)) {
      return false;
    }
    out = bytes_.subspan(at_, length);
    at_ += length;
    return true;
  }

  bool text(std::string_view field, std::string& out) {
    std::span<const std::byte> raw_text;
    if (!raw(field, "a text", kMaxTextBytes, raw_text)) {
      return false;
    }
    out.clear();
    out.reserve(raw_text.size());
    for (const std::byte byte : raw_text) {
      out += static_cast<char>(byte);
    }
    return true;
  }

  bool window(std::string_view field, std::vector<std::byte>& out) {
    std::span<const std::byte> raw_window;
    if (!raw(field, "a window", kMaxWindowBytes, raw_window)) {
      return false;
    }
    out.assign(raw_window.begin(), raw_window.end());
    return true;
  }

  bool presence(std::string_view field, bool& present) {
    const std::size_t field_at = at_;
    std::uint8_t flag = 0;
    if (!u8(field, flag)) {
      return false;
    }
    if (flag > 1) {
      return fail(DecodeFault::kInvalid, where(field, field_at) + ": presence flag " +
                                             std::to_string(flag) + ", neither 0 nor 1");
    }
    present = flag == 1;
    return true;
  }

  bool optional_u8(std::string_view field, std::optional<std::uint8_t>& out) {
    bool present = false;
    if (!presence(field, present)) {
      return false;
    }
    out.reset();
    std::uint8_t value = 0;
    if (present && !u8(field, value)) {
      return false;
    }
    if (present) {
      out = value;
    }
    return true;
  }

  bool optional_u32(std::string_view field, std::optional<std::uint32_t>& out) {
    bool present = false;
    if (!presence(field, present)) {
      return false;
    }
    out.reset();
    std::uint32_t value = 0;
    if (present && !u32(field, value)) {
      return false;
    }
    if (present) {
      out = value;
    }
    return true;
  }

  bool optional_u64(std::string_view field, std::optional<std::uint64_t>& out) {
    bool present = false;
    if (!presence(field, present)) {
      return false;
    }
    out.reset();
    std::uint64_t value = 0;
    if (present && !u64(field, value)) {
      return false;
    }
    if (present) {
      out = value;
    }
    return true;
  }

 private:
  static std::string where(std::string_view field, std::size_t at) {
    return "field '" + std::string(field) + "' at byte " + std::to_string(at);
  }

  bool need(std::string_view field, std::uint64_t count) {
    if (count <= remaining()) {
      return true;
    }
    return fail(DecodeFault::kTruncated, where(field, at_) + " needs " + std::to_string(count) +
                                             " byte(s), " + std::to_string(remaining()) +
                                             " remain");
  }

  bool fail(DecodeFault fault, std::string detail) {
    error_ = DecodeError{fault, std::move(detail)};
    return false;
  }

  std::span<const std::byte> bytes_;
  std::size_t at_;
  DecodeError error_;
};

bool get_provenance(Reader& reader, Provenance& provenance) {
  std::optional<std::uint8_t> isa;
  std::optional<std::uint8_t> integrity;
  bool duty_present = false;
  std::uint32_t oracle_period = 0;
  std::uint32_t integrity_period = 0;
  bool workload_present = false;
  std::string workload;
  const bool read =
      reader.optional_u32("logical_cpu", provenance.logical_cpu) &&
      reader.optional_u32("physical_core", provenance.physical_core) &&
      reader.optional_u32("numa_node", provenance.numa_node) &&
      reader.optional_u64("buffer_address", provenance.buffer_address) &&
      reader.optional_u64("buffer_offset", provenance.buffer_offset) &&
      reader.optional_u64("iteration", provenance.iteration) &&
      reader.presence("workload", workload_present) &&
      (!workload_present || reader.text("workload", workload)) &&
      reader.optional_u32("workload_version", provenance.workload_version) &&
      reader.optional_u64("seed", provenance.seed) && reader.optional_u8("isa", isa) &&
      reader.optional_u8("input_integrity", integrity) && reader.presence("duty", duty_present) &&
      (!duty_present ||
       (reader.u32("duty", oracle_period) && reader.u32("duty", integrity_period)));
  if (!read) {
    return false;
  }
  if (workload_present) {
    provenance.workload = std::move(workload);
  }
  if (isa.has_value()) {
    provenance.isa = static_cast<simd::IsaPath>(*isa);
  }
  if (integrity.has_value()) {
    provenance.input_integrity = static_cast<InputIntegrity>(*integrity);
  }
  if (duty_present) {
    provenance.duty = DutyCycle{oracle_period, integrity_period};
  }
  return true;
}

std::optional<ErrorRecord> get_exact(Reader& reader, Provenance provenance) {
  ExactBitCorruption record;
  record.provenance = std::move(provenance);
  std::uint8_t side = 0;
  bool has_difference = false;
  if (!reader.u8("side", side) || !reader.text("check", record.check) ||
      !reader.u32("line_bytes", record.line_bytes) ||
      !reader.presence("difference", has_difference)) {
    return std::nullopt;
  }
  record.side = static_cast<Side>(side);
  if (!has_difference) {
    return record;
  }
  Difference difference;
  std::uint8_t domain = 0;
  if (!reader.u8("domain", domain) || !reader.u64("first_offset", difference.first_offset) ||
      !reader.u64("last_offset", difference.last_offset) ||
      !reader.u64("popcount", difference.popcount) ||
      !reader.u64("window_offset", difference.window_offset) ||
      !reader.window("expected", difference.expected) ||
      !reader.window("actual", difference.actual)) {
    return std::nullopt;
  }
  difference.domain = static_cast<Domain>(domain);
  record.difference = std::move(difference);
  return record;
}

std::optional<ErrorRecord> get_numerical(Reader& reader, Provenance provenance) {
  NumericalVerificationFailure record;
  record.provenance = std::move(provenance);
  bool has_condition = false;
  double condition = 0.0;
  if (!reader.text("check", record.check) || !reader.f64("residual", record.residual) ||
      !reader.f64("tolerance", record.tolerance) ||
      !reader.presence("condition_number", has_condition) ||
      (has_condition && !reader.f64("condition_number", condition)) ||
      !reader.optional_u64("element", record.element)) {
    return std::nullopt;
  }
  if (has_condition) {
    record.condition_number = condition;
  }
  return record;
}

std::optional<ErrorRecord> get_not_performed(Reader& reader, Provenance provenance) {
  VerificationNotPerformed record;
  record.provenance = std::move(provenance);
  if (!reader.text("what", record.what) || !reader.text("why", record.why) ||
      !reader.u64("blocks_compared", record.blocks_compared)) {
    return std::nullopt;
  }
  return record;
}

std::string longer_than_allowed(std::string_view what, std::size_t size) {
  return std::string(what) + " of " + std::to_string(size) + " bytes, more than the " +
         std::to_string(kMaxTextBytes) + " a record carries";
}

std::optional<std::string> broken_difference_rule(const Difference& difference) {
  if (difference.domain > Domain::kDerived) {
    return "difference domain " + std::to_string(static_cast<unsigned>(difference.domain)) +
           " is neither stored nor derived";
  }
  if (difference.popcount == 0) {
    return std::string("difference popcount 0: a difference in which no bit differs");
  }
  if (difference.first_offset > difference.last_offset) {
    return "difference first offset " + std::to_string(difference.first_offset) +
           " after its last offset " + std::to_string(difference.last_offset);
  }
  const std::uint64_t word_start = difference.first_offset / kWordBytes * kWordBytes;
  if (difference.window_offset != word_start) {
    return "difference window offset " + std::to_string(difference.window_offset) +
           ", not the first offset rounded down to a word (" + std::to_string(word_start) + ")";
  }
  if (difference.expected.size() != difference.actual.size()) {
    return "difference windows of " + std::to_string(difference.expected.size()) + " and " +
           std::to_string(difference.actual.size()) + " bytes, not the same length";
  }
  if (difference.expected.empty()) {
    return std::string("difference windows empty: no byte to show");
  }
  if (difference.expected.size() > kMaxWindowBytes) {
    return "difference windows of " + std::to_string(difference.expected.size()) +
           " bytes, more than the " + std::to_string(kMaxWindowBytes) + " a record carries";
  }
  return std::nullopt;
}

std::optional<std::string> broken_kind_rule(const ExactBitCorruption& record) {
  if (record.side > Side::kInput) {
    return "side " + std::to_string(static_cast<unsigned>(record.side)) +
           " is neither output nor input";
  }
  if (record.check.empty()) {
    return std::string("an exact corruption that names no check");
  }
  if (record.check.size() > kMaxTextBytes) {
    return longer_than_allowed("check", record.check.size());
  }
  if (record.line_bytes < kWordBytes) {
    return "line size of " + std::to_string(record.line_bytes) + " bytes, less than a word (" +
           std::to_string(kWordBytes) + ")";
  }
  if (!std::has_single_bit(record.line_bytes)) {
    return "line size of " + std::to_string(record.line_bytes) + " bytes, not a power of two";
  }
  if (record.difference.has_value()) {
    return broken_difference_rule(*record.difference);
  }
  return std::nullopt;
}

std::optional<std::string> broken_kind_rule(const NumericalVerificationFailure& record) {
  if (record.check.size() > kMaxTextBytes) {
    return longer_than_allowed("check", record.check.size());
  }
  if (!std::isfinite(record.tolerance)) {
    return "tolerance " + shortest(record.tolerance) + " is not finite";
  }
  if (record.tolerance <= 0.0) {
    return "tolerance " + shortest(record.tolerance) + " is not strictly positive";
  }
  return std::nullopt;
}

std::optional<std::string> broken_kind_rule(const VerificationNotPerformed& record) {
  if (record.what.size() > kMaxTextBytes) {
    return longer_than_allowed("what", record.what.size());
  }
  if (record.why.size() > kMaxTextBytes) {
    return longer_than_allowed("why", record.why.size());
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> broken_rule(const ErrorRecord& record) {
  const Provenance& provenance = provenance_of(record);
  if (provenance.workload.has_value() && provenance.workload->size() > kMaxTextBytes) {
    return longer_than_allowed("workload name", provenance.workload->size());
  }
  if (provenance.isa.has_value() &&
      static_cast<std::size_t>(*provenance.isa) >= simd::kIsaPathCount) {
    return "ISA path " + std::to_string(static_cast<unsigned>(*provenance.isa)) +
           " is none of the " + std::to_string(simd::kIsaPathCount);
  }
  if (provenance.input_integrity.has_value() &&
      *provenance.input_integrity > InputIntegrity::kNotChecked) {
    return "input integrity " + std::to_string(static_cast<unsigned>(*provenance.input_integrity)) +
           " is none of its four values";
  }
  // std::visit rather than a chain of get_if: at -O3, GCC's -Wnull-dereference
  // cannot see that the last alternative is held when the others are not.
  return std::visit([](const auto& kind) { return broken_kind_rule(kind); }, record);
}

std::string describe(const DecodeError& error) {
  std::string text{describe(error.fault)};
  if (!error.detail.empty()) {
    text += ": ";
    text += error.detail;
  }
  return text;
}

std::vector<std::byte> encode(const ErrorRecord& record) {
  Writer writer;
  writer.u8(kRecordFormatVersion);
  writer.u8(static_cast<std::uint8_t>(record.index() + 1));
  put_provenance(writer, provenance_of(record));
  std::visit([&writer](const auto& kind) { put_kind(writer, kind); }, record);
  return writer.take();
}

core::Result<ErrorRecord, DecodeError> decode(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return DecodeError{DecodeFault::kEmpty,
                       "no bytes: a record is at least its format version and its kind"};
  }
  const auto version = std::to_integer<std::uint8_t>(bytes[0]);
  if (version != kRecordFormatVersion) {
    return DecodeError{DecodeFault::kVersion,
                       "byte 0 is format version " + std::to_string(version) +
                           "; this build reads version " + std::to_string(kRecordFormatVersion)};
  }
  if (bytes.size() < kHeaderBytes) {
    return DecodeError{DecodeFault::kTruncated, "the bytes end after the format version"};
  }
  const auto kind = std::to_integer<std::uint8_t>(bytes[1]);
  if (kind < kExactKind || kind > kNotPerformedKind) {
    return DecodeError{DecodeFault::kKind,
                       "byte 1 is kind " + std::to_string(kind) + "; the kinds are 1, 2 and 3"};
  }
  Reader reader(bytes);
  Provenance provenance;
  if (!get_provenance(reader, provenance)) {
    return reader.error();
  }
  std::optional<ErrorRecord> record;
  if (kind == kExactKind) {
    record = get_exact(reader, std::move(provenance));
  } else if (kind == kNumericalKind) {
    record = get_numerical(reader, std::move(provenance));
  } else {
    record = get_not_performed(reader, std::move(provenance));
  }
  if (!record.has_value()) {
    return reader.error();
  }
  if (auto rule = broken_rule(*record)) {
    return DecodeError{DecodeFault::kInvalid, std::move(*rule)};
  }
  if (reader.remaining() != 0) {
    return DecodeError{DecodeFault::kTrailing, std::to_string(reader.remaining()) +
                                                   " byte(s) after the record, which ends at "
                                                   "byte " +
                                                   std::to_string(reader.at())};
  }
  return std::move(*record);
}

}  // namespace loadforge::verification
