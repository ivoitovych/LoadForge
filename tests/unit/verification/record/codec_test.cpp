// SPDX-License-Identifier: GPL-3.0-or-later
//
// The codec beyond the contract's suite: the layout codec.hpp writes down,
// byte for byte; each way decode() refuses bytes, with the field and offset
// its detail names; the canonical-encoding property; and broken_rule() read
// against is_valid().

#include "verification/record/codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/verification/record_contract.hpp"
#include "verification/record.hpp"
#include "verification/record/format.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

// The layout as codec.hpp states it, written here from that text rather than
// from codec.cpp, so the two are compared.
class Layout {
 public:
  Layout& le(std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
      bytes.push_back(static_cast<std::byte>(value >> (8 * index)));
    }
    return *this;
  }
  Layout& u8(std::uint64_t value) { return le(value, 1); }
  Layout& u32(std::uint64_t value) { return le(value, 4); }
  Layout& u64(std::uint64_t value) { return le(value, 8); }
  Layout& text(std::string_view value) {
    u64(value.size());
    for (const char c : value) {
      bytes.push_back(static_cast<std::byte>(c));
    }
    return *this;
  }
  Bytes bytes;
};

DecodeError decode_error(const Bytes& bytes) {
  const auto decoded = decode(bytes);
  EXPECT_FALSE(decoded.has_value());
  return decoded.has_value() ? DecodeError{} : decoded.error();
}

TEST(RecordCodecLayout, ARecordWithNoProvenanceIsTheDocumentedBytes) {
  const VerificationNotPerformed record = contracts::record_samples::not_performed();
  Layout expected;
  expected.u8(2).u8(3);
  for (int field = 0; field < 15; ++field) {
    expected.u8(0);
  }
  expected.text("oracle over compression units").text("no unit was sampled").u64(0);
  EXPECT_EQ(encode(record), expected.bytes);
}

TEST(RecordCodecLayout, EveryProvenanceFieldAndEveryExactFieldInItsPlace) {
  const ExactBitCorruption record = contracts::record_samples::exact();
  Layout expected;
  expected.u8(2).u8(1);
  expected.u8(1).u32(11).u8(1).u32(5).u8(1).u32(1);
  expected.u8(1).u64(0x7f0012340000ULL).u8(1).u64(0x40000).u8(1).u64(123456789);
  expected.u8(1).text("compression").u8(1).u32(3).u8(1).u64(0xFEEDFACECAFEBEEFULL);
  expected.u8(1).u8(1).u8(1).u8(2).u8(1).u32(4).u32(16);
  // Version 2: the package, the resolved decomposition, the footprint.
  expected.u8(1).u32(1).u8(1).u32(64).u8(1).u64(std::uint64_t{32} << 20U);
  expected.u8(1).text("input against its regeneration").u32(128).u8(1);
  expected.u8(0).u64(0x1003).u64(0x1003).u64(1).u64(0x1000);
  expected.u64(8).u64(0).u64(8).u64(0x10ULL << 24U);
  EXPECT_EQ(encode(record), expected.bytes);
}

TEST(RecordCodecLayout, ANumericalRecordCarriesItsDoublesAsBitPatterns) {
  NumericalVerificationFailure record;
  record.check = "momentum";
  record.residual = std::bit_cast<double>(0xFFF8000000000000ULL);
  record.tolerance = 0.5;
  record.condition_number = std::numeric_limits<double>::infinity();
  Layout expected;
  expected.u8(2).u8(2);
  for (int field = 0; field < 15; ++field) {
    expected.u8(0);
  }
  expected.text("momentum")
      .u64(0xFFF8000000000000ULL)
      .u64(std::bit_cast<std::uint64_t>(0.5))
      .u8(1)
      .u64(std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()))
      .u8(0);
  EXPECT_EQ(encode(record), expected.bytes);
}

TEST(RecordCodecRoundTrip, ANanKeepsItsPayloadAndASignedZeroItsSign) {
  NumericalVerificationFailure record = contracts::record_samples::numerical();
  record.residual = std::bit_cast<double>(0x7FF8000000000123ULL);
  record.condition_number = -0.0;
  const auto decoded = decode(encode(record));
  ASSERT_TRUE(decoded.has_value());
  const auto& back = std::get<NumericalVerificationFailure>(decoded.value());
  EXPECT_EQ(std::bit_cast<std::uint64_t>(back.residual), 0x7FF8000000000123ULL);
  ASSERT_TRUE(back.condition_number.has_value());
  EXPECT_TRUE(std::signbit(*back.condition_number));
}

TEST(RecordCodecRoundTrip, EachOptionalFieldAloneSurvives) {
  // One field present at a time: a reader that confused two adjacent fields
  // would put the value in the wrong place and fail the equality.
  const Provenance full = contracts::record_samples::full_provenance();
  const std::vector<void (*)(Provenance&, const Provenance&)> setters = {
      [](Provenance& p, const Provenance& f) { p.logical_cpu = f.logical_cpu; },
      [](Provenance& p, const Provenance& f) { p.physical_core = f.physical_core; },
      [](Provenance& p, const Provenance& f) { p.numa_node = f.numa_node; },
      [](Provenance& p, const Provenance& f) { p.buffer_address = f.buffer_address; },
      [](Provenance& p, const Provenance& f) { p.buffer_offset = f.buffer_offset; },
      [](Provenance& p, const Provenance& f) { p.iteration = f.iteration; },
      [](Provenance& p, const Provenance& f) { p.workload = f.workload; },
      [](Provenance& p, const Provenance& f) { p.workload_version = f.workload_version; },
      [](Provenance& p, const Provenance& f) { p.seed = f.seed; },
      [](Provenance& p, const Provenance& f) { p.isa = f.isa; },
      [](Provenance& p, const Provenance& f) { p.input_integrity = f.input_integrity; },
      [](Provenance& p, const Provenance& f) { p.duty = f.duty; },
      [](Provenance& p, const Provenance& f) { p.physical_package = f.physical_package; },
      [](Provenance& p, const Provenance& f) { p.decomposition = f.decomposition; },
      [](Provenance& p, const Provenance& f) { p.footprint_bytes = f.footprint_bytes; },
  };
  for (std::size_t index = 0; index < setters.size(); ++index) {
    VerificationNotPerformed record = contracts::record_samples::not_performed();
    setters[index](record.provenance, full);
    const auto decoded = decode(encode(record));
    ASSERT_TRUE(decoded.has_value()) << "field " << index;
    EXPECT_EQ(decoded.value(), ErrorRecord{record}) << "field " << index;
  }
}

TEST(RecordCodecDecode, NothingIsEmptyAndSaysSo) {
  const DecodeError error = decode_error({});
  EXPECT_EQ(error.fault, DecodeFault::kEmpty);
  EXPECT_EQ(error.detail, "no bytes: a record is at least its format version and its kind");
}

TEST(RecordCodecDecode, AnUnknownVersionIsTheAnswerEvenForOneByte) {
  for (const std::uint8_t version : {0, 1, 3, 255}) {
    const DecodeError error = decode_error({static_cast<std::byte>(version)});
    EXPECT_EQ(error.fault, DecodeFault::kVersion) << int{version};
    EXPECT_EQ(error.detail, "byte 0 is format version " + std::to_string(version) +
                                "; this build reads version 2");
  }
}

TEST(RecordCodecDecode, AVersionAloneIsTruncated) {
  const DecodeError error = decode_error({std::byte{2}});
  EXPECT_EQ(error.fault, DecodeFault::kTruncated);
  EXPECT_EQ(error.detail, "the bytes end after the format version");
}

TEST(RecordCodecDecode, AnUnknownKindNamesItself) {
  for (const std::uint8_t kind : {0, 4, 200}) {
    const DecodeError error = decode_error({std::byte{2}, static_cast<std::byte>(kind)});
    EXPECT_EQ(error.fault, DecodeFault::kKind);
    EXPECT_EQ(error.detail,
              "byte 1 is kind " + std::to_string(kind) + "; the kinds are 1, 2 and 3");
  }
}

TEST(RecordCodecDecode, TruncationNamesTheFieldItsOffsetAndWhatRemains) {
  Bytes bytes = encode(contracts::record_samples::exact());
  // Byte 2 is logical_cpu's presence flag, 3..6 its value.
  bytes.resize(5);
  const DecodeError error = decode_error(bytes);
  EXPECT_EQ(error.fault, DecodeFault::kTruncated);
  EXPECT_EQ(error.detail, "field 'logical_cpu' at byte 3 needs 4 byte(s), 2 remain");
  EXPECT_EQ(describe(error), "truncated: field 'logical_cpu' at byte 3 needs 4 byte(s), 2 remain");
}

TEST(RecordCodecDecode, APresenceFlagThatIsNeitherZeroNorOneIsInvalidAtEveryField) {
  static constexpr std::array<std::string_view, 15> kFields = {
      "logical_cpu",     "physical_core", "numa_node",        "buffer_address", "buffer_offset",
      "iteration",       "workload",      "workload_version", "seed",           "isa",
      "input_integrity", "duty",          "physical_package", "decomposition",  "footprint_bytes"};
  for (std::size_t field = 0; field < kFields.size(); ++field) {
    for (const std::uint8_t flag : {2, 0xFF}) {
      Bytes bytes = encode(contracts::record_samples::not_performed());
      bytes[2 + field] = static_cast<std::byte>(flag);
      const DecodeError error = decode_error(bytes);
      EXPECT_EQ(error.fault, DecodeFault::kInvalid) << kFields[field];
      EXPECT_EQ(error.detail, "field '" + std::string(kFields[field]) + "' at byte " +
                                  std::to_string(2 + field) + ": presence flag " +
                                  std::to_string(flag) + ", neither 0 nor 1");
    }
  }
}

TEST(RecordCodecDecode, TheKindsOwnPresenceFlagsAreCheckedToo) {
  ExactBitCorruption exact = contracts::record_samples::exact();
  exact.difference.reset();
  Bytes bytes = encode(exact);
  bytes.back() = std::byte{2};
  EXPECT_EQ(decode_error(bytes).fault, DecodeFault::kInvalid);
  EXPECT_NE(decode_error(bytes).detail.find("'difference'"), std::string::npos);

  NumericalVerificationFailure bounded = contracts::record_samples::numerical();
  bounded.condition_number.reset();
  bounded.element.reset();
  bytes = encode(bounded);
  bytes[bytes.size() - 2] = std::byte{3};
  EXPECT_NE(decode_error(bytes).detail.find("'condition_number'"), std::string::npos);
  bytes = encode(bounded);
  bytes.back() = std::byte{3};
  EXPECT_NE(decode_error(bytes).detail.find("'element'"), std::string::npos);
}

TEST(RecordCodecDecode, ATextLengthPastTheLimitIsRefusedBeforeItIsTrusted) {
  // Byte 17 is the length of `what` in a record without provenance.
  const Bytes base = encode(contracts::record_samples::not_performed());
  Bytes bytes = base;
  bytes[17] = std::byte{0x01};
  bytes[18] = std::byte{0x04};  // 1025
  DecodeError error = decode_error(bytes);
  EXPECT_EQ(error.fault, DecodeFault::kInvalid);
  EXPECT_EQ(error.detail,
            "field 'what' at byte 17: a text of 1025 bytes, more than the 1024 a record carries");

  bytes = base;
  for (std::size_t at = 17; at < 25; ++at) {
    bytes[at] = std::byte{0xFF};
  }
  error = decode_error(bytes);
  EXPECT_EQ(error.fault, DecodeFault::kInvalid) << "no allocation of 2^64 - 1 bytes is attempted";

  bytes = base;
  bytes[17] = std::byte{0x00};
  bytes[18] = std::byte{0x04};  // 1024: allowed, and more than remain
  error = decode_error(bytes);
  EXPECT_EQ(error.fault, DecodeFault::kTruncated);
  EXPECT_EQ(error.detail.rfind("field 'what' at byte 25 needs 1024 byte(s), ", 0), 0U)
      << error.detail;
}

TEST(RecordCodecDecode, AWindowLengthPastTheLimitIsRefusedForEitherSide) {
  const Bytes base = encode(contracts::record_samples::exact());
  // The tail is [u64 length][8 bytes] twice: expected, then actual.
  for (const std::size_t from_end : {std::size_t{32}, std::size_t{16}}) {
    Bytes bytes = base;
    bytes[bytes.size() - from_end] = std::byte{65};
    const DecodeError error = decode_error(bytes);
    EXPECT_EQ(error.fault, DecodeFault::kInvalid);
    EXPECT_NE(error.detail.find("a window of 65 bytes, more than the 64 a record carries"),
              std::string::npos)
        << error.detail;
    EXPECT_NE(error.detail.find(from_end == 32 ? "'expected'" : "'actual'"), std::string::npos);
  }
}

TEST(RecordCodecDecode, EachUnnamedEnumeratorIsInvalidNotGuessed) {
  ExactBitCorruption record = contracts::record_samples::exact();
  record.provenance.isa = static_cast<simd::IsaPath>(4);
  EXPECT_EQ(decode_error(encode(record)).detail, "ISA path 4 is none of the 4");
  record = contracts::record_samples::exact();
  record.provenance.input_integrity = static_cast<InputIntegrity>(4);
  EXPECT_EQ(decode_error(encode(record)).detail, "input integrity 4 is none of its four values");
  record = contracts::record_samples::exact();
  record.side = static_cast<Side>(2);
  EXPECT_EQ(decode_error(encode(record)).detail, "side 2 is neither output nor input");
  record = contracts::record_samples::exact();
  record.difference->domain = static_cast<Domain>(2);
  EXPECT_EQ(decode_error(encode(record)).detail,
            "difference domain 2 is neither stored nor derived");
}

TEST(RecordCodecDecode, TrailingBytesAreCountedAndTheRecordsEndNamed) {
  Bytes bytes = encode(contracts::record_samples::not_performed());
  const std::size_t end = bytes.size();
  bytes.push_back(std::byte{0});
  bytes.push_back(std::byte{0});
  const DecodeError error = decode_error(bytes);
  EXPECT_EQ(error.fault, DecodeFault::kTrailing);
  EXPECT_EQ(error.detail, "2 byte(s) after the record, which ends at byte " + std::to_string(end));
}

TEST(RecordCodecDecode, AnInvalidRecordIsJudgedBeforeItsTrailingBytes) {
  NumericalVerificationFailure bounded = contracts::record_samples::numerical();
  bounded.tolerance = 0.0;
  Bytes bytes = encode(bounded);
  bytes.push_back(std::byte{0});
  EXPECT_EQ(decode_error(bytes).fault, DecodeFault::kInvalid);
}

TEST(RecordCodecDecode, WhateverDecodesReencodesToTheSameBytes) {
  // The canonical-encoding property codec.hpp states, over every valid sample
  // and every single-bit change of each.
  for (const ErrorRecord& record : contracts::record_samples::valid_samples()) {
    const Bytes bytes = encode(record);
    for (std::size_t at = 0; at < bytes.size(); ++at) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        Bytes changed = bytes;
        changed[at] ^= static_cast<std::byte>(1U << bit);
        const auto decoded = decode(changed);
        if (decoded.has_value()) {
          ASSERT_TRUE(is_valid(decoded.value())) << "byte " << at << " bit " << bit;
          ASSERT_EQ(encode(decoded.value()), changed) << "byte " << at << " bit " << bit;
        }
      }
    }
  }
}

// --------------------------------------------------------------------------
// broken_rule()
// --------------------------------------------------------------------------

TEST(RecordCodecRules, BrokenRuleAgreesWithIsValidOnEverySample) {
  for (const ErrorRecord& record : contracts::record_samples::valid_samples()) {
    EXPECT_EQ(broken_rule(record), std::nullopt) << kind_name(record);
  }
  for (const ErrorRecord& record : contracts::record_samples::invalid_samples()) {
    const std::optional<std::string> rule = broken_rule(record);
    ASSERT_TRUE(rule.has_value()) << kind_name(record);
    EXPECT_FALSE(rule->empty());
  }
}

TEST(RecordCodecRules, EachRuleIsNamedInWords) {
  using contracts::record_samples::exact;
  using contracts::record_samples::not_performed;
  using contracts::record_samples::numerical;
  const auto rule = [](const ErrorRecord& record) { return broken_rule(record).value_or(""); };

  ExactBitCorruption record = exact();
  record.provenance.workload = std::string(kMaxTextBytes + 1, 'w');
  EXPECT_EQ(rule(record), "workload name of 1025 bytes, more than the 1024 a record carries");
  record = exact();
  record.check.clear();
  EXPECT_EQ(rule(record), "an exact corruption that names no check");
  record = exact();
  record.check = std::string(kMaxTextBytes + 1, 'k');
  EXPECT_EQ(rule(record), "check of 1025 bytes, more than the 1024 a record carries");
  record = exact();
  record.line_bytes = 4;
  EXPECT_EQ(rule(record), "line size of 4 bytes, less than a word (8)");
  record.line_bytes = 96;
  EXPECT_EQ(rule(record), "line size of 96 bytes, not a power of two");
  record = exact();
  record.difference->popcount = 0;
  EXPECT_EQ(rule(record), "difference popcount 0: a difference in which no bit differs");
  record = exact();
  record.difference->last_offset = 0x1002;
  EXPECT_EQ(rule(record), "difference first offset 4099 after its last offset 4098");
  record = exact();
  record.difference->window_offset = 0x1003;
  EXPECT_EQ(rule(record),
            "difference window offset 4099, not the first offset rounded down to a word (4096)");
  record = exact();
  record.difference->actual.pop_back();
  EXPECT_EQ(rule(record), "difference windows of 8 and 7 bytes, not the same length");
  record = exact();
  record.difference->expected.clear();
  record.difference->actual.clear();
  EXPECT_EQ(rule(record), "difference windows empty: no byte to show");
  record = exact(kMaxWindowBytes + 1);
  EXPECT_EQ(rule(record), "difference windows of 65 bytes, more than the 64 a record carries");

  NumericalVerificationFailure bounded = numerical();
  bounded.check = std::string(kMaxTextBytes + 1, 'c');
  EXPECT_EQ(rule(bounded), "check of 1025 bytes, more than the 1024 a record carries");
  bounded = numerical();
  bounded.tolerance = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(rule(bounded), "tolerance nan is not finite");
  bounded.tolerance = -std::numeric_limits<double>::infinity();
  EXPECT_EQ(rule(bounded), "tolerance -inf is not finite");
  bounded.tolerance = -0.0;
  EXPECT_EQ(rule(bounded), "tolerance -0 is not strictly positive");

  VerificationNotPerformed unrun = not_performed();
  unrun.what = std::string(kMaxTextBytes + 1, 'q');
  EXPECT_EQ(rule(unrun), "what of 1025 bytes, more than the 1024 a record carries");
  unrun = not_performed();
  unrun.why = std::string(kMaxTextBytes + 1, 'q');
  EXPECT_EQ(rule(unrun), "why of 1025 bytes, more than the 1024 a record carries");
}

// External review 2026-10-20 12:51 UTC, W1-01: a numerical FAILURE has a NaN
// residual or one past its tolerance. The decoder is the trust boundary, so
// each way to describe a passing check is refused by is_valid(), broken_rule()
// and decode() alike, and never reaches a consumer's is_wrong_result().
TEST(RecordCodecNumericalFailure, AResidualInsideTheToleranceIsACheckThatPassedAndDoesNotDecode) {
  using contracts::record_samples::numerical;
  constexpr double kInf = std::numeric_limits<double>::infinity();
  NumericalVerificationFailure record = numerical();
  record.tolerance = 1.0;
  record.check = "C versus AB";
  record.condition_number.reset();
  record.element.reset();
  record.provenance = Provenance{};
  for (const double residual : {0.0, -0.0, 0.5, 1.0, std::nextafter(1.0, 0.0), -1.0, -1.0e-300,
                                -2.0, -std::numeric_limits<double>::max(), -kInf}) {
    record.residual = residual;
    const ErrorRecord wrapped = record;
    EXPECT_FALSE(is_valid(wrapped)) << shortest(residual);
    ASSERT_TRUE(broken_rule(wrapped).has_value()) << shortest(residual);
    EXPECT_NE(broken_rule(wrapped)->find("does not exceed tolerance 1"), std::string::npos)
        << *broken_rule(wrapped);
    const auto decoded = decode(encode(wrapped));
    ASSERT_FALSE(decoded.has_value()) << shortest(residual);
    EXPECT_EQ(decoded.error().fault, DecodeFault::kInvalid);
    EXPECT_NE(decoded.error().detail.find("a check that passed"), std::string::npos)
        << decoded.error().detail;
  }
}

TEST(RecordCodecNumericalFailure, ANanOrAResidualPastTheToleranceDecodesAndIsAWrongResult) {
  using contracts::record_samples::numerical;
  NumericalVerificationFailure record = numerical();
  record.tolerance = 1.0;
  for (const double residual :
       {std::nextafter(1.0, 2.0), 1.5, std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
        std::bit_cast<double>(0x7FF8000000000123ULL), std::bit_cast<double>(0xFFF8000000000000ULL),
        std::bit_cast<double>(0xFFF8000000000123ULL)}) {
    record.residual = residual;
    const ErrorRecord wrapped = record;
    EXPECT_TRUE(is_valid(wrapped)) << shortest(residual);
    EXPECT_EQ(broken_rule(wrapped), std::nullopt) << shortest(residual);
    const auto decoded = decode(encode(wrapped));
    ASSERT_TRUE(decoded.has_value()) << shortest(residual);
    EXPECT_TRUE(is_wrong_result(decoded.value()));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(
                  std::get<NumericalVerificationFailure>(decoded.value()).residual),
              std::bit_cast<std::uint64_t>(residual))
        << "the residual's bits, a NaN's sign and payload included, are kept as they were";
  }
}

// W1-05: an exact difference agrees with its own excerpt.
TEST(RecordCodecDifferenceWindow, AClaimedFirstDifferenceThatTheWindowShowsIdenticalDoesNotDecode) {
  using contracts::record_samples::exact;
  ExactBitCorruption record = exact();
  record.difference->actual = record.difference->expected;
  const ErrorRecord wrapped = record;
  EXPECT_FALSE(is_valid(wrapped));
  EXPECT_EQ(broken_rule(wrapped),
            "difference first offset 4099: expected and actual bytes there are the same");
  const auto decoded = decode(encode(wrapped));
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().fault, DecodeFault::kInvalid);

  // The review's counterexample: popcount 1, offsets 0, identical windows.
  ExactBitCorruption review;
  review.check = "input against its regeneration";
  review.difference = Difference{Domain::kStored, 0, 0, 1, 0, {std::byte{0x5A}}, {std::byte{0x5A}}};
  EXPECT_FALSE(is_valid(ErrorRecord{review}));
  EXPECT_FALSE(decode(encode(ErrorRecord{review})).has_value());
  review.difference->actual[0] = std::byte{0x5B};
  EXPECT_TRUE(is_valid(ErrorRecord{review}));
  EXPECT_TRUE(decode(encode(ErrorRecord{review})).has_value());
}

TEST(RecordCodecDifferenceWindow, ADifferenceElsewhereInTheWindowDoesNotStandInForTheFirstOffset) {
  using contracts::record_samples::exact;
  ExactBitCorruption record = exact();
  record.difference->actual[3] = record.difference->expected[3];
  record.difference->actual[5] = std::byte{0x01};
  EXPECT_FALSE(is_valid(ErrorRecord{record}));
  EXPECT_EQ(broken_rule(ErrorRecord{record}),
            "difference first offset 4099: expected and actual bytes there are the same");
}

TEST(RecordCodecDifferenceWindow, AWindowThatDoesNotCoverTheFirstOffsetDoesNotDecode) {
  using contracts::record_samples::exact;
  const ExactBitCorruption record = exact(3);  // offsets 0x1000..0x1002; first_offset is 0x1003
  const ErrorRecord wrapped = record;
  EXPECT_FALSE(is_valid(wrapped));
  EXPECT_EQ(broken_rule(wrapped),
            "difference first offset 4099 outside its window of 3 bytes at offset 4096");
  const auto decoded = decode(encode(wrapped));
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().fault, DecodeFault::kInvalid);
}

TEST(RecordCodecDifferenceWindow, ALastOffsetInsideTheWindowMustDifferThereAndOutsideNeedNot) {
  using contracts::record_samples::exact;
  ExactBitCorruption record = exact();
  record.difference->last_offset = 0x1006;  // inside the window, bytes identical
  EXPECT_FALSE(is_valid(ErrorRecord{record}));
  EXPECT_EQ(broken_rule(ErrorRecord{record}),
            "difference last offset 4102: expected and actual bytes there are the same");
  EXPECT_FALSE(decode(encode(ErrorRecord{record})).has_value());
  record.difference->actual[6] = std::byte{0x80};
  EXPECT_TRUE(is_valid(ErrorRecord{record}));
  EXPECT_TRUE(decode(encode(ErrorRecord{record})).has_value());
  // Beyond the window the excerpt cannot say, and the record is accepted.
  record.difference->last_offset = 0x1008;
  record.difference->actual[6] = record.difference->expected[6];
  EXPECT_TRUE(is_valid(ErrorRecord{record}));
  EXPECT_TRUE(decode(encode(ErrorRecord{record})).has_value());
}

TEST(RecordCodecDifferenceWindow, WindowMembershipIsJudgedAtBothEdges) {
  const Difference difference = contracts::record_samples::difference();  // 0x1000, 8 bytes
  EXPECT_FALSE(in_window(difference, 0x0FFF)) << "before the window";
  EXPECT_TRUE(in_window(difference, 0x1000));
  EXPECT_TRUE(in_window(difference, 0x1007));
  EXPECT_FALSE(in_window(difference, 0x1008)) << "past the window";
  EXPECT_FALSE(differs_at(difference, 0x0FFF));
  EXPECT_FALSE(differs_at(difference, 0x1008));
  EXPECT_FALSE(differs_at(difference, 0x1002)) << "inside, and the same";
  EXPECT_TRUE(differs_at(difference, 0x1003));
}

TEST(RecordCodecRules, ADecodeErrorWithoutDetailIsItsFaultAlone) {
  EXPECT_EQ(describe(DecodeError{DecodeFault::kTrailing, ""}), "trailing bytes");
  EXPECT_EQ(describe(DecodeError{DecodeFault::kKind, "byte 1 is kind 9"}),
            "unknown record kind: byte 1 is kind 9");
}

}  // namespace
}  // namespace loadforge::verification
