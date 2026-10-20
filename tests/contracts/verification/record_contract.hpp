// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_RECORD_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_RECORD_CONTRACT_HPP

// Provider-side suite for the record encoding in src/verification/record.hpp
// (ADR 0003, 0004).
//
// RecordCodecContract<Subject>: Subject::encode and Subject::decode have the
// signatures of verification::encode and verification::decode.
//
// The sample records are built here, by the suite, from the contract's words;
// a provider adds its own beside them, never instead of them.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/record.hpp"

namespace loadforge::contracts {

namespace record_samples {

using verification::DutyCycle;
using verification::ErrorRecord;
using verification::ExactBitCorruption;
using verification::InputIntegrity;
using verification::NumericalVerificationFailure;
using verification::Provenance;
using verification::Side;
using verification::VerificationNotPerformed;

inline Provenance full_provenance() {
  Provenance provenance;
  provenance.logical_cpu = 11;
  provenance.physical_core = 5;
  provenance.numa_node = 1;
  provenance.buffer_address = 0x7f0012340000ULL;
  provenance.buffer_offset = 0x40000;
  provenance.iteration = 123456789;
  provenance.workload = "compression";
  provenance.workload_version = 3;
  provenance.seed = 0xFEEDFACECAFEBEEFULL;
  provenance.isa = simd::IsaPath::kAvx2;
  provenance.input_integrity = InputIntegrity::kCorruptDuringConsumption;
  provenance.duty = DutyCycle{4, 16};
  return provenance;
}

inline verification::Difference difference(std::size_t window = 8) {
  verification::Difference difference;
  difference.domain = verification::Domain::kStored;
  difference.first_offset = 0x1003;
  difference.last_offset = 0x1003;
  difference.popcount = 1;
  difference.window_offset = 0x1000;
  difference.expected.assign(window, std::byte{0x00});
  difference.actual.assign(window, std::byte{0x00});
  difference.actual[3] = std::byte{0x10};
  return difference;
}

inline ExactBitCorruption exact(std::size_t window = 8) {
  ExactBitCorruption record;
  record.provenance = full_provenance();
  record.side = Side::kInput;
  record.check = "input against its regeneration";
  record.difference = difference(window);
  record.line_bytes = 128;
  return record;
}

inline NumericalVerificationFailure numerical() {
  NumericalVerificationFailure record;
  record.provenance = full_provenance();
  record.check = "componentwise |C - AB|";
  record.residual = 3.5e-12;
  record.tolerance = 1.25e-13;
  record.condition_number = 42.0;
  record.element = 9001;
  return record;
}

inline VerificationNotPerformed not_performed() {
  VerificationNotPerformed record;
  record.what = "oracle over compression units";
  record.why = "no unit was sampled";
  record.blocks_compared = 0;
  return record;
}

/// Valid records, chosen for their edges: every field set, none set, the
/// largest window, the longest text, a NaN and an infinite residual, every
/// enumerator.
inline std::vector<ErrorRecord> valid_samples() {
  std::vector<ErrorRecord> samples;
  samples.emplace_back(exact());
  ExactBitCorruption bare;
  bare.check = "c";
  bare.difference = verification::Difference{
      verification::Domain::kDerived, 0, 1000, 7, 0, {std::byte{0xFF}}, {std::byte{0x00}}};
  bare.line_bytes = 8;
  samples.emplace_back(bare);
  ExactBitCorruption malformed = exact();
  malformed.side = Side::kOutput;
  malformed.check = "compressed stream malformed at byte 517";
  malformed.difference.reset();
  samples.emplace_back(malformed);
  ExactBitCorruption widest = exact(verification::kMaxWindowBytes);
  widest.side = Side::kOutput;
  widest.check = std::string(verification::kMaxTextBytes, 'k');
  widest.provenance.workload = std::string(verification::kMaxTextBytes, 'w');
  widest.provenance.isa = simd::IsaPath::kNeon;
  widest.provenance.input_integrity = InputIntegrity::kNotChecked;
  samples.emplace_back(widest);
  samples.emplace_back(numerical());
  NumericalVerificationFailure nan = numerical();
  nan.residual = std::numeric_limits<double>::quiet_NaN();
  nan.condition_number.reset();
  nan.element.reset();
  nan.provenance = Provenance{};
  samples.emplace_back(nan);
  NumericalVerificationFailure infinite = numerical();
  infinite.residual = std::numeric_limits<double>::infinity();
  infinite.tolerance = std::numeric_limits<double>::denorm_min();
  infinite.check = std::string(verification::kMaxTextBytes, 'c');
  samples.emplace_back(infinite);
  // A failure is a residual past the tolerance or a NaN, whatever the NaN's
  // sign or payload; the smallest residual past the tolerance is one.
  NumericalVerificationFailure barely = numerical();
  barely.residual = std::nextafter(barely.tolerance, std::numeric_limits<double>::infinity());
  samples.emplace_back(barely);
  NumericalVerificationFailure negative_nan = numerical();
  negative_nan.residual = std::bit_cast<double>(0xFFF8000000000123ULL);
  samples.emplace_back(negative_nan);
  // A last offset past the window, and one inside it that differs.
  ExactBitCorruption beyond = exact();
  beyond.difference->last_offset = 0x1003 + 5000;
  samples.emplace_back(beyond);
  ExactBitCorruption inside = exact();
  inside.difference->last_offset = 0x1006;
  inside.difference->actual[6] = std::byte{0x01};
  samples.emplace_back(inside);
  samples.emplace_back(not_performed());
  VerificationNotPerformed counted = not_performed();
  counted.blocks_compared = std::numeric_limits<std::uint64_t>::max();
  counted.provenance = full_provenance();
  counted.provenance.isa = simd::IsaPath::kScalar;
  counted.provenance.input_integrity = InputIntegrity::kIntact;
  counted.what = std::string(verification::kMaxTextBytes, 'q');
  counted.why = "";
  samples.emplace_back(counted);
  return samples;
}

/// Records the contract forbids, one rule broken in each.
inline std::vector<ErrorRecord> invalid_samples() {
  std::vector<ErrorRecord> samples;
  ExactBitCorruption record = exact();
  record.difference->popcount = 0;
  samples.emplace_back(record);
  record = exact();
  record.side = static_cast<Side>(7);
  samples.emplace_back(record);
  record = exact();
  record.difference->domain = static_cast<verification::Domain>(7);
  samples.emplace_back(record);
  record = exact();
  record.provenance.isa = static_cast<simd::IsaPath>(9);
  samples.emplace_back(record);
  record = exact();
  record.provenance.input_integrity = static_cast<InputIntegrity>(9);
  samples.emplace_back(record);
  record = exact();
  record.check.clear();
  samples.emplace_back(record);
  record = exact();
  record.check = std::string(verification::kMaxTextBytes + 1, 'k');
  samples.emplace_back(record);
  record = exact();
  record.difference->last_offset = record.difference->first_offset - 1;
  samples.emplace_back(record);
  record = exact();
  record.line_bytes = 4;
  samples.emplace_back(record);
  record = exact();
  record.line_bytes = 96;
  samples.emplace_back(record);
  record = exact();
  record.difference.reset();
  record.line_bytes = 0;
  samples.emplace_back(record);
  record = exact();
  record.difference->window_offset = record.difference->first_offset;
  samples.emplace_back(record);
  record = exact();
  record.difference->actual.pop_back();
  samples.emplace_back(record);
  record = exact();
  record.difference->expected.clear();
  record.difference->actual.clear();
  samples.emplace_back(record);
  samples.emplace_back(exact(verification::kMaxWindowBytes + 1));
  record = exact();
  record.provenance.workload = std::string(verification::kMaxTextBytes + 1, 'w');
  samples.emplace_back(record);
  NumericalVerificationFailure bounded = numerical();
  bounded.tolerance = 0.0;
  samples.emplace_back(bounded);
  bounded.tolerance = -1.0;
  samples.emplace_back(bounded);
  bounded.tolerance = std::numeric_limits<double>::infinity();
  samples.emplace_back(bounded);
  bounded.tolerance = std::numeric_limits<double>::quiet_NaN();
  samples.emplace_back(bounded);
  bounded = numerical();
  bounded.check = std::string(verification::kMaxTextBytes + 1, 'c');
  samples.emplace_back(bounded);
  // A numerical FAILURE has a NaN residual or one past the tolerance (external
  // review 2026-10-20 12:51 UTC, W1-01): a residual inside it is a pass, a
  // negative one is no measurement.
  for (const double residual :
       {0.0, -0.0, 1.0e-300, bounded.tolerance / 2, bounded.tolerance, -bounded.tolerance, -1.0,
        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::max()}) {
    bounded = numerical();
    bounded.residual = residual;
    samples.emplace_back(bounded);
  }
  // An exact difference agrees with its own window (W1-05): it covers
  // first_offset and the bytes differ there, and at last_offset when that lies
  // inside the window.
  record = exact();
  record.difference->actual[3] = record.difference->expected[3];
  samples.emplace_back(record);
  record = exact(3);
  samples.emplace_back(record);
  record = exact(4);
  record.difference->actual[3] = std::byte{0x00};
  record.difference->actual[0] = std::byte{0x01};
  samples.emplace_back(record);
  record = exact();
  record.difference->last_offset = 0x1006;
  samples.emplace_back(record);
  record = exact();
  record.difference->last_offset = 0x1006;
  record.difference->actual[6] = std::byte{0x01};
  record.difference->actual[3] = std::byte{0x00};
  samples.emplace_back(record);
  VerificationNotPerformed unrun = not_performed();
  unrun.what = std::string(verification::kMaxTextBytes + 1, 'q');
  samples.emplace_back(unrun);
  unrun = not_performed();
  unrun.why = std::string(verification::kMaxTextBytes + 1, 'q');
  samples.emplace_back(unrun);
  return samples;
}

/// Equality that treats a NaN residual as equal to itself: the round trip must
/// keep a NaN a NaN, and operator== cannot say so.
inline bool same(const ErrorRecord& lhs, const ErrorRecord& rhs) {
  const auto* left = std::get_if<NumericalVerificationFailure>(&lhs);
  const auto* right = std::get_if<NumericalVerificationFailure>(&rhs);
  if (left != nullptr && right != nullptr && std::isnan(left->residual) &&
      std::isnan(right->residual)) {
    NumericalVerificationFailure a = *left;
    NumericalVerificationFailure b = *right;
    a.residual = 0.0;
    b.residual = 0.0;
    return a == b;
  }
  return lhs == rhs;
}

}  // namespace record_samples

template <typename Subject>
class RecordCodecContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(RecordCodecContract);

TYPED_TEST_P(RecordCodecContract, EveryValidRecordSurvivesTheRoundTrip) {
  for (const verification::ErrorRecord& record : record_samples::valid_samples()) {
    ASSERT_TRUE(verification::is_valid(record)) << "the sample itself must be valid";
    const auto decoded = TypeParam::decode(TypeParam::encode(record));
    ASSERT_TRUE(decoded.has_value())
        << verification::kind_name(record) << ": " << decoded.error().detail;
    EXPECT_TRUE(record_samples::same(decoded.value(), record)) << verification::kind_name(record);
  }
}

TYPED_TEST_P(RecordCodecContract, TheFirstTwoBytesAreTheVersionAndTheKind) {
  const std::vector<verification::ErrorRecord> records = {
      record_samples::exact(), record_samples::numerical(), record_samples::not_performed()};
  for (std::size_t kind = 0; kind < records.size(); ++kind) {
    const std::vector<std::byte> bytes = TypeParam::encode(records[kind]);
    ASSERT_GE(bytes.size(), 2U);
    EXPECT_EQ(bytes[0], std::byte{verification::kRecordFormatVersion});
    EXPECT_EQ(bytes[1], static_cast<std::byte>(kind + 1));
  }
}

TYPED_TEST_P(RecordCodecContract, EncodingIsDeterministic) {
  for (const verification::ErrorRecord& record : record_samples::valid_samples()) {
    EXPECT_EQ(TypeParam::encode(record), TypeParam::encode(record));
  }
  EXPECT_NE(TypeParam::encode(record_samples::exact()),
            TypeParam::encode(record_samples::exact(16)))
      << "different records, different bytes";
}

TYPED_TEST_P(RecordCodecContract, NothingIsRefusedAsEmpty) {
  const auto decoded = TypeParam::decode(std::span<const std::byte>{});
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().fault, verification::DecodeFault::kEmpty);
}

TYPED_TEST_P(RecordCodecContract, AnUnknownVersionOrKindIsRefusedAsSuch) {
  std::vector<std::byte> bytes = TypeParam::encode(record_samples::exact());
  bytes[0] = std::byte{verification::kRecordFormatVersion + 1};
  EXPECT_EQ(TypeParam::decode(bytes).error().fault, verification::DecodeFault::kVersion);
  bytes = TypeParam::encode(record_samples::exact());
  for (const std::byte kind : {std::byte{0}, std::byte{4}, std::byte{0xFF}}) {
    bytes[1] = kind;
    EXPECT_EQ(TypeParam::decode(bytes).error().fault, verification::DecodeFault::kKind);
  }
}

TYPED_TEST_P(RecordCodecContract, EveryStrictPrefixIsTruncatedAndExtraBytesAreTrailing) {
  for (const verification::ErrorRecord& record : record_samples::valid_samples()) {
    const std::vector<std::byte> bytes = TypeParam::encode(record);
    for (std::size_t length = 1; length < bytes.size(); ++length) {
      const auto decoded = TypeParam::decode(std::span(bytes).first(length));
      ASSERT_FALSE(decoded.has_value()) << "prefix " << length << " of " << bytes.size();
      EXPECT_EQ(decoded.error().fault, verification::DecodeFault::kTruncated)
          << "prefix " << length;
    }
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    const auto decoded = TypeParam::decode(longer);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().fault, verification::DecodeFault::kTrailing);
  }
}

TYPED_TEST_P(RecordCodecContract, AnInvalidRecordNeverDecodesToItself) {
  for (const verification::ErrorRecord& record : record_samples::invalid_samples()) {
    ASSERT_FALSE(verification::is_valid(record));
    const auto decoded = TypeParam::decode(TypeParam::encode(record));
    ASSERT_FALSE(decoded.has_value()) << verification::kind_name(record);
    EXPECT_EQ(decoded.error().fault, verification::DecodeFault::kInvalid);
    EXPECT_FALSE(decoded.error().detail.empty());
  }
}

TYPED_TEST_P(RecordCodecContract, ArbitraryBytesAreAnsweredNeverCrashedOn) {
  // A channel carrying garbage is what this tool exists to detect; the
  // decoder's part is to answer, valid record or error, for any input.
  for (std::uint64_t stream = 0; stream < 2000; ++stream) {
    std::vector<std::byte> bytes(stream % 97);
    rng::fill_stream(1, stream, bytes);
    if (bytes.size() >= 2 && stream % 2 == 0) {
      bytes[0] = std::byte{verification::kRecordFormatVersion};
      bytes[1] = static_cast<std::byte>(1 + stream % 3);
    }
    const auto decoded = TypeParam::decode(bytes);
    if (decoded.has_value()) {
      EXPECT_TRUE(verification::is_valid(decoded.value())) << "stream " << stream;
    }
  }
}

REGISTER_TYPED_TEST_SUITE_P(RecordCodecContract, EveryValidRecordSurvivesTheRoundTrip,
                            TheFirstTwoBytesAreTheVersionAndTheKind, EncodingIsDeterministic,
                            NothingIsRefusedAsEmpty, AnUnknownVersionOrKindIsRefusedAsSuch,
                            EveryStrictPrefixIsTruncatedAndExtraBytesAreTrailing,
                            AnInvalidRecordNeverDecodesToItself,
                            ArbitraryBytesAreAnsweredNeverCrashedOn);

}  // namespace loadforge::contracts

#endif
