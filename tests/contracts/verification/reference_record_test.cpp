// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference encoding of error records, run against the codec suite,
// and the tests of the definitions src/verification/record.hpp carries. The
// verification-core stream writes the real codec; this one exists to prove
// the suite can be passed.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "contracts/verification/record_contract.hpp"
#include "verification/record.hpp"

namespace loadforge::contracts {
namespace {

using verification::DecodeError;
using verification::DecodeFault;
using verification::ErrorRecord;
using verification::ExactBitCorruption;
using verification::NumericalVerificationFailure;
using verification::Provenance;
using verification::VerificationNotPerformed;

class Writer {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }
  void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void blob(std::span<const std::byte> data) {
    u64(data.size());
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  void text(const std::string& value) { blob(std::as_bytes(std::span(value))); }
  template <typename T, typename Put>
  void optional(const std::optional<T>& value, Put put) {
    u8(value.has_value() ? 1 : 0);
    if (value.has_value()) {
      put(*value);
    }
  }
  std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
  bool u8(std::uint8_t& out) {
    if (at_ >= bytes_.size()) {
      return false;
    }
    out = std::to_integer<std::uint8_t>(bytes_[at_++]);
    return true;
  }
  bool u64(std::uint64_t& out) {
    out = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte = 0;
      if (!u8(byte)) {
        return false;
      }
      out |= std::uint64_t{byte} << shift;
    }
    return true;
  }
  bool u32(std::uint32_t& out) {
    std::uint64_t wide = 0;
    if (!u64(wide)) {
      return false;
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
  }
  bool f64(double& out) {
    std::uint64_t bits = 0;
    if (!u64(bits)) {
      return false;
    }
    out = std::bit_cast<double>(bits);
    return true;
  }
  bool blob(std::vector<std::byte>& out) {
    std::uint64_t size = 0;
    if (!u64(size) || size > bytes_.size() - at_) {
      return false;
    }
    out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(at_),
               bytes_.begin() + static_cast<std::ptrdiff_t>(at_ + size));
    at_ += size;
    return true;
  }
  bool text(std::string& out) {
    std::vector<std::byte> raw;
    if (!blob(raw)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
  }
  template <typename T, typename Get>
  bool optional(std::optional<T>& out, Get get) {
    std::uint8_t present = 0;
    if (!u8(present)) {
      return false;
    }
    if (present == 0) {
      out.reset();
      return true;
    }
    T value{};
    if (!get(value)) {
      return false;
    }
    out = value;
    return true;
  }
  [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
};

void put_provenance(Writer& w, const Provenance& p) {
  auto u32 = [&w](std::uint32_t v) { w.u64(v); };
  auto u64 = [&w](std::uint64_t v) { w.u64(v); };
  w.optional(p.logical_cpu, u32);
  w.optional(p.physical_core, u32);
  w.optional(p.numa_node, u32);
  w.optional(p.buffer_address, u64);
  w.optional(p.buffer_offset, u64);
  w.optional(p.iteration, u64);
  w.optional(p.workload, [&w](const std::string& v) { w.text(v); });
  w.optional(p.workload_version, u32);
  w.optional(p.seed, u64);
  w.optional(p.isa, [&w](simd::IsaPath v) { w.u8(static_cast<std::uint8_t>(v)); });
  w.optional(p.input_integrity,
             [&w](verification::InputIntegrity v) { w.u8(static_cast<std::uint8_t>(v)); });
  w.optional(p.duty, [&w](verification::DutyCycle v) {
    w.u64(v.oracle_period);
    w.u64(v.integrity_period);
  });
  w.optional(p.physical_package, u32);
  w.optional(p.decomposition, u32);
  w.optional(p.footprint_bytes, u64);
}

bool get_provenance(Reader& r, Provenance& p) {
  auto u32 = [&r](std::uint32_t& v) { return r.u32(v); };
  auto u64 = [&r](std::uint64_t& v) { return r.u64(v); };
  return r.optional(p.logical_cpu, u32) && r.optional(p.physical_core, u32) &&
         r.optional(p.numa_node, u32) && r.optional(p.buffer_address, u64) &&
         r.optional(p.buffer_offset, u64) && r.optional(p.iteration, u64) &&
         r.optional(p.workload, [&r](std::string& v) { return r.text(v); }) &&
         r.optional(p.workload_version, u32) && r.optional(p.seed, u64) &&
         r.optional(p.isa,
                    [&r](simd::IsaPath& v) {
                      std::uint8_t raw = 0;
                      const bool ok = r.u8(raw);
                      v = static_cast<simd::IsaPath>(raw);
                      return ok;
                    }) &&
         r.optional(p.input_integrity,
                    [&r](verification::InputIntegrity& v) {
                      std::uint8_t raw = 0;
                      const bool ok = r.u8(raw);
                      v = static_cast<verification::InputIntegrity>(raw);
                      return ok;
                    }) &&
         r.optional(p.duty,
                    [&r](verification::DutyCycle& v) {
                      return r.u32(v.oracle_period) && r.u32(v.integrity_period);
                    }) &&
         r.optional(p.physical_package, u32) && r.optional(p.decomposition, u32) &&
         r.optional(p.footprint_bytes, u64);
}

std::vector<std::byte> reference_encode(const ErrorRecord& record) {
  Writer w;
  w.u8(verification::kRecordFormatVersion);
  w.u8(static_cast<std::uint8_t>(record.index() + 1));
  put_provenance(w, verification::provenance_of(record));
  if (const auto* exact = std::get_if<ExactBitCorruption>(&record)) {
    w.u8(static_cast<std::uint8_t>(exact->side));
    w.text(exact->check);
    w.u64(exact->line_bytes);
    w.optional(exact->difference, [&w](const verification::Difference& d) {
      w.u8(static_cast<std::uint8_t>(d.domain));
      w.u64(d.first_offset);
      w.u64(d.last_offset);
      w.u64(d.popcount);
      w.u64(d.window_offset);
      w.blob(d.expected);
      w.blob(d.actual);
    });
  } else if (const auto* numerical = std::get_if<NumericalVerificationFailure>(&record)) {
    w.text(numerical->check);
    w.f64(numerical->residual);
    w.f64(numerical->tolerance);
    w.optional(numerical->condition_number, [&w](double v) { w.f64(v); });
    w.optional(numerical->element, [&w](std::uint64_t v) { w.u64(v); });
  } else {
    const auto& unrun = std::get<VerificationNotPerformed>(record);
    w.text(unrun.what);
    w.text(unrun.why);
    w.u64(unrun.blocks_compared);
  }
  return w.take();
}

core::Result<ErrorRecord, DecodeError> reference_decode(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return DecodeError{DecodeFault::kEmpty, "no bytes"};
  }
  Reader r(bytes);
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  r.u8(version);
  if (version != verification::kRecordFormatVersion) {
    return DecodeError{DecodeFault::kVersion, "version " + std::to_string(version)};
  }
  if (!r.u8(kind)) {
    return DecodeError{DecodeFault::kTruncated, "no kind"};
  }
  if (kind < 1 || kind > 3) {
    return DecodeError{DecodeFault::kKind, "kind " + std::to_string(kind)};
  }
  Provenance provenance;
  bool ok = get_provenance(r, provenance);
  std::optional<ErrorRecord> record;
  if (ok && kind == 1) {
    ExactBitCorruption exact;
    exact.provenance = provenance;
    std::uint8_t side = 0;
    ok = r.u8(side) && r.text(exact.check) && r.u32(exact.line_bytes) &&
         r.optional(exact.difference, [&r](verification::Difference& d) {
           std::uint8_t domain = 0;
           const bool read = r.u8(domain) && r.u64(d.first_offset) && r.u64(d.last_offset) &&
                             r.u64(d.popcount) && r.u64(d.window_offset) && r.blob(d.expected) &&
                             r.blob(d.actual);
           d.domain = static_cast<verification::Domain>(domain);
           return read;
         });
    exact.side = static_cast<verification::Side>(side);
    record = exact;
  } else if (ok && kind == 2) {
    NumericalVerificationFailure numerical;
    numerical.provenance = provenance;
    ok = r.text(numerical.check) && r.f64(numerical.residual) && r.f64(numerical.tolerance) &&
         r.optional(numerical.condition_number, [&r](double& v) { return r.f64(v); }) &&
         r.optional(numerical.element, [&r](std::uint64_t& v) { return r.u64(v); });
    record = numerical;
  } else if (ok) {
    VerificationNotPerformed unrun;
    unrun.provenance = provenance;
    ok = r.text(unrun.what) && r.text(unrun.why) && r.u64(unrun.blocks_compared);
    record = unrun;
  }
  if (!ok) {
    return DecodeError{DecodeFault::kTruncated, "the bytes end inside the record"};
  }
  if (!r.done()) {
    return DecodeError{DecodeFault::kTrailing, "bytes after the record"};
  }
  if (!verification::is_valid(*record)) {
    return DecodeError{DecodeFault::kInvalid, "the record breaks the contract's rules"};
  }
  return *record;
}

struct ReferenceCodec {
  static std::vector<std::byte> encode(const ErrorRecord& record) {
    return reference_encode(record);
  }
  static core::Result<ErrorRecord, DecodeError> decode(std::span<const std::byte> bytes) {
    return reference_decode(bytes);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, RecordCodecContract, ::testing::Types<ReferenceCodec>);

}  // namespace
}  // namespace loadforge::contracts
