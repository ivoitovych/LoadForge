// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for exact comparison: corruption of each shape injected into a buffer as
// the runner lays one out -- line-aligned, regenerated from (seed, index) --
// and the record asserted by WHICH classification it produced, never merely
// that something was caught (verification.md §6; M2a exit criterion 2). T5b:
// a comparison that cannot be made produces VerificationNotPerformed, never a
// pass.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/checksum/compare.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t kUnitBytes = 4096;

/// A unit's input as generate() would write it, and the record a regeneration
/// check makes of `actual` against it.
class Injection : public ::testing::Test {
 protected:
  Injection() : truth_(unit_input()), actual_(truth_) {}

  static Bytes unit_input() {
    Bytes input(kUnitBytes);
    rng::fill_stream(0x5EED, 42, input);
    return input;
  }

  void flip(std::size_t byte, unsigned bit) { actual_[byte] ^= static_cast<std::byte>(1U << bit); }

  std::optional<ErrorRecord> record(std::uint32_t line_bytes = 64) const {
    Subject subject;
    subject.provenance.iteration = 42;
    subject.provenance.seed = 0x5EED;
    subject.provenance.buffer_address = 0x7f0000400000ULL;
    subject.side = Side::kInput;
    subject.check = "input against its regeneration";
    subject.line_bytes = line_bytes;
    return to_record(compare_exact(truth_, actual_, Domain::kStored), subject);
  }

  /// The classification of the record, which must be a valid exact corruption.
  std::optional<BitClass> bit_class(std::uint32_t line_bytes = 64) const {
    const std::optional<ErrorRecord> made = record(line_bytes);
    EXPECT_TRUE(made.has_value());
    if (!made.has_value()) {
      return std::nullopt;
    }
    EXPECT_TRUE(is_valid(*made));
    const auto* exact = std::get_if<ExactBitCorruption>(&*made);
    EXPECT_NE(exact, nullptr) << "a wrong input is a wrong result";
    return exact == nullptr ? std::nullopt : classify(*exact);
  }

  [[nodiscard]] const Difference& difference() const {
    static const Difference none;
    const std::optional<ErrorRecord> made = record();
    if (!made.has_value()) {
      return none;
    }
    stored_ = std::get<ExactBitCorruption>(*made).difference.value_or(Difference{});
    return stored_;
  }

  Bytes truth_;
  Bytes actual_;
  mutable Difference stored_;
};

TEST_F(Injection, NothingInjectedNothingFound) { EXPECT_EQ(record(), std::nullopt); }

TEST_F(Injection, ASingleBitAnywhereIsASingleBit) {
  for (const std::size_t byte : {std::size_t{0}, std::size_t{7}, std::size_t{8}, std::size_t{63},
                                 std::size_t{64}, kUnitBytes - 1}) {
    for (const unsigned bit : {0U, 7U}) {
      actual_ = truth_;
      flip(byte, bit);
      EXPECT_EQ(bit_class(), BitClass::kSingleBit) << "byte " << byte << " bit " << bit;
      EXPECT_EQ(difference().popcount, 1U);
      EXPECT_EQ(difference().first_offset, byte);
      EXPECT_EQ(difference().last_offset, byte);
    }
  }
}

TEST_F(Injection, TwoBitsInOneByteAreMultipleBitsInOneWord) {
  flip(1000, 0);
  flip(1000, 5);
  EXPECT_EQ(bit_class(), BitClass::kMultiBitInWord);
  EXPECT_EQ(difference().popcount, 2U);
}

TEST_F(Injection, TheFirstAndLastByteOfOneWordAreMultipleBitsInOneWord) {
  flip(1024, 3);
  flip(1031, 3);
  EXPECT_EQ(bit_class(), BitClass::kMultiBitInWord);
}

TEST_F(Injection, AWholeWordCorruptedIsMultipleBitsInOneWord) {
  for (std::size_t byte = 2048; byte < 2056; ++byte) {
    actual_[byte] = ~truth_[byte];
  }
  EXPECT_EQ(bit_class(), BitClass::kMultiBitInWord);
  EXPECT_EQ(difference().popcount, 64U);
}

TEST_F(Injection, AdjacentBytesAcrossAWordBoundaryAreWithinOneLine) {
  flip(1031, 0);
  flip(1032, 0);
  EXPECT_EQ(bit_class(), BitClass::kWithinCacheLine);
}

TEST_F(Injection, AWholeCacheLineCorruptedIsWithinOneLine) {
  // IMPLEMENTATION M2a's "whole-cache-line" class.
  constexpr std::size_t kLine = 64;
  for (std::size_t byte = 3 * kLine; byte < 4 * kLine; ++byte) {
    actual_[byte] = ~truth_[byte];
  }
  EXPECT_EQ(bit_class(), BitClass::kWithinCacheLine);
  EXPECT_EQ(difference().popcount, 512U);
  EXPECT_EQ(difference().window_offset, 192U);
  EXPECT_EQ(difference().expected.size(), kMaxWindowBytes) << "the whole line, and no more";
}

TEST_F(Injection, AdjacentBytesAcrossALineBoundaryAreAcrossLines) {
  flip(63, 2);
  flip(64, 2);
  EXPECT_EQ(bit_class(), BitClass::kAcrossCacheLines);
}

TEST_F(Injection, TheFirstAndLastByteOfTheUnitAreAcrossLines) {
  flip(0, 0);
  flip(kUnitBytes - 1, 7);
  EXPECT_EQ(bit_class(), BitClass::kAcrossCacheLines);
  EXPECT_EQ(difference().first_offset, 0U);
  EXPECT_EQ(difference().last_offset, kUnitBytes - 1);
}

TEST_F(Injection, TheTopologysLineSizeDecidesTheClass) {
  // The same two flips, one line on a 128-byte-line machine and two on a
  // 64-byte one: the record's line_bytes, never an assumed 64.
  flip(63, 2);
  flip(64, 2);
  EXPECT_EQ(bit_class(64), BitClass::kAcrossCacheLines);
  EXPECT_EQ(bit_class(128), BitClass::kWithinCacheLine);
}

TEST_F(Injection, ADerivedComparisonIsLabelledDerived) {
  flip(5, 5);
  Subject subject;
  subject.side = Side::kOutput;
  subject.check = "decoded output against input";
  const std::optional<ErrorRecord> made =
      to_record(compare_exact(truth_, actual_, Domain::kDerived), subject);
  ASSERT_TRUE(made.has_value());
  EXPECT_EQ(std::get<ExactBitCorruption>(*made).difference->domain, Domain::kDerived);
}

TEST_F(Injection, ATruncatedOutputIsAWrongResultWithNoClassification) {
  actual_.pop_back();
  const std::optional<ErrorRecord> made = record();
  ASSERT_TRUE(made.has_value());
  EXPECT_TRUE(is_wrong_result(*made));
  EXPECT_EQ(classify(std::get<ExactBitCorruption>(*made)), std::nullopt)
      << "absence of bit evidence, never a zero popcount";
}

TEST_F(Injection, AComparisonOverNothingIsNotPerformedNeverAPass) {
  // T5b: the check made impossible -- nothing was handed to it to compare.
  truth_.clear();
  actual_.clear();
  const std::optional<ErrorRecord> made = record();
  ASSERT_TRUE(made.has_value()) << "an unrun check must not read as no finding";
  EXPECT_TRUE(std::holds_alternative<VerificationNotPerformed>(*made));
  EXPECT_FALSE(is_wrong_result(*made)) << "and it is never routed as a wrong result";
  EXPECT_EQ(std::get<VerificationNotPerformed>(*made).blocks_compared, 0U);
}

}  // namespace
}  // namespace loadforge::verification
