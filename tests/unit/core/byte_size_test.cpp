// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/byte_size.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace loadforge::core {
namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = kKiB * 1024;
constexpr std::uint64_t kGiB = kMiB * 1024;
constexpr std::uint64_t kTiB = kGiB * 1024;

ByteSize parsed(std::string_view text) {
  const auto r = parse_byte_size(text);
  EXPECT_TRUE(r.has_value()) << "unexpectedly rejected: '" << text << "'";
  return r.value_or(ByteSize::bytes(0));
}

ParseError rejected(std::string_view text) {
  const auto r = parse_byte_size(text);
  EXPECT_FALSE(r.has_value()) << "unexpectedly accepted: '" << text << "'";
  return r.has_value() ? ParseError::Empty : r.error();
}

TEST(ParseByteSize, AcceptsEveryBinaryUnit) {
  EXPECT_EQ(parsed("512B").byte_count(), 512U);
  EXPECT_EQ(parsed("4KiB").byte_count(), 4 * kKiB);
  EXPECT_EQ(parsed("512MiB").byte_count(), 512 * kMiB);
  EXPECT_EQ(parsed("8GiB").byte_count(), 8 * kGiB);
  EXPECT_EQ(parsed("2TiB").byte_count(), 2 * kTiB);
}

TEST(ParseByteSize, BareNumberIsAByteCount) {
  EXPECT_TRUE(parsed("1024").is_absolute());
  EXPECT_EQ(parsed("1024").byte_count(), 1024U);
  EXPECT_EQ(parsed("0").byte_count(), 0U);
}

TEST(ParseByteSize, AcceptsPercentage) {
  const ByteSize seventy = parsed("70%");
  EXPECT_FALSE(seventy.is_absolute());
  EXPECT_EQ(seventy.kind(), ByteSize::Kind::Fraction);
  EXPECT_EQ(seventy.percent_of_available(), 70U);
  EXPECT_EQ(parsed("1%").percent_of_available(), 1U);
  EXPECT_EQ(parsed("100%").percent_of_available(), 100U);
}

ByteSize pct(std::uint64_t whole) {
  const auto r = ByteSize::percent(whole);
  EXPECT_TRUE(r.has_value()) << "percent(" << whole << ") unexpectedly rejected";
  return r.value_or(ByteSize::bytes(0));
}

TEST(ByteSize, InvalidPercentagesAreUnrepresentable) {
  // Previously these were constructible even though the parser refused them,
  // so to_string(percent(0)) produced "0%" -- text parse_byte_size rejects.
  EXPECT_FALSE(ByteSize::percent(0).has_value());
  EXPECT_FALSE(ByteSize::percent(101).has_value());
  EXPECT_FALSE(ByteSize::percent(1000).has_value());
  EXPECT_EQ(ByteSize::percent(0).error(), ParseError::OutOfRange);
  EXPECT_TRUE(ByteSize::percent(1).has_value());
  EXPECT_TRUE(ByteSize::percent(100).has_value());
}

TEST(ByteSize, KindReportsBothAlternatives) {
  EXPECT_EQ(ByteSize::bytes(1).kind(), ByteSize::Kind::Absolute);
  EXPECT_TRUE(ByteSize::bytes(1).is_absolute());
  EXPECT_EQ(pct(50).kind(), ByteSize::Kind::Fraction);
  EXPECT_FALSE(pct(50).is_absolute());
}

TEST(ByteSize, AccessorsThrowRatherThanReturnAPlausibleWrongAnswer) {
  // percent(70).byte_count() used to return 70 -- "70 bytes" -- silently.
  EXPECT_THROW((void)pct(70).byte_count(), std::bad_variant_access);
  EXPECT_THROW((void)ByteSize::bytes(70).percent_of_available(), std::bad_variant_access);
}

TEST(ParseByteSize, RejectsPercentageOutsideOneToHundred) {
  EXPECT_EQ(rejected("0%"), ParseError::OutOfRange);
  EXPECT_EQ(rejected("101%"), ParseError::OutOfRange);
  EXPECT_EQ(rejected("1000%"), ParseError::OutOfRange);
}

TEST(ParseByteSize, ToleratesSurroundingWhitespace) {
  EXPECT_EQ(parsed("  8GiB ").byte_count(), 8 * kGiB);
  EXPECT_EQ(parsed("\t70%\t").percent_of_available(), 70U);
}

TEST(ParseByteSize, RejectsEmptyAndWhitespaceOnly) {
  EXPECT_EQ(rejected(""), ParseError::Empty);
  EXPECT_EQ(rejected("  "), ParseError::Empty);
}

TEST(ParseByteSize, RejectsNegative) { EXPECT_EQ(rejected("-1GiB"), ParseError::NegativeValue); }

TEST(ParseByteSize, RejectsMalformedNumber) {
  EXPECT_EQ(rejected("GiB"), ParseError::InvalidNumber);
  EXPECT_EQ(rejected("abc"), ParseError::InvalidNumber);
  EXPECT_EQ(rejected("%"), ParseError::InvalidNumber);
}

TEST(ParseByteSize, RejectsDecimalUnits) {
  // Deliberate: a 7% memory-sizing mistake is exactly the kind that goes
  // unnoticed until a run starts swapping.
  EXPECT_EQ(rejected("8GB"), ParseError::UnknownUnit);
  EXPECT_EQ(rejected("512MB"), ParseError::UnknownUnit);
  EXPECT_EQ(rejected("4K"), ParseError::UnknownUnit);
  EXPECT_EQ(rejected("8gib"), ParseError::UnknownUnit);  // case sensitive
}

TEST(ParseByteSize, RejectsNumberTooLargeForUint64) {
  EXPECT_EQ(rejected("99999999999999999999999B"), ParseError::OutOfRange);
}

TEST(ParseByteSize, RejectsValueThatOverflowsWhenScaled) {
  EXPECT_EQ(rejected("16777217TiB"), ParseError::OutOfRange);
}

TEST(ParseByteSize, AcceptsLargestValueThatStillFits) {
  constexpr std::uint64_t kMaxTiB = std::numeric_limits<std::uint64_t>::max() / kTiB;
  EXPECT_EQ(parsed(std::to_string(kMaxTiB) + "TiB").byte_count(), kMaxTiB * kTiB);
}

TEST(ParseByteSize, ValueOrYieldsFallbackForRejectedInput) {
  EXPECT_EQ(parse_byte_size("nonsense").value_or(ByteSize::bytes(7)), ByteSize::bytes(7));
  EXPECT_EQ(parse_byte_size("8GiB").value_or(ByteSize::bytes(7)), ByteSize::bytes(8 * kGiB));
}

TEST(ByteSizeToString, ChoosesLargestExactUnit) {
  EXPECT_EQ(to_string(ByteSize::bytes(8 * kGiB)), "8GiB");
  EXPECT_EQ(to_string(ByteSize::bytes(512 * kMiB)), "512MiB");
  EXPECT_EQ(to_string(ByteSize::bytes(4 * kKiB)), "4KiB");
  EXPECT_EQ(to_string(ByteSize::bytes(2 * kTiB)), "2TiB");
  EXPECT_EQ(to_string(ByteSize::bytes(512)), "512B");
}

TEST(ByteSizeToString, HandlesZeroAndIndivisible) {
  EXPECT_EQ(to_string(ByteSize::bytes(0)), "0B");
  EXPECT_EQ(to_string(ByteSize::bytes(1536)), "1536B");  // 1.5KiB is not exact
}

TEST(ByteSizeToString, RendersPercentage) { EXPECT_EQ(to_string(pct(70)), "70%"); }

TEST(ByteSize, ComparesByKindAndValue) {
  EXPECT_EQ(ByteSize::bytes(100), ByteSize::bytes(100));
  EXPECT_NE(ByteSize::bytes(100), ByteSize::bytes(101));
  // Two percentages compare by value, not merely by kind.
  EXPECT_EQ(pct(70), pct(70));
  EXPECT_NE(pct(70), pct(50));
  // 100 bytes and 100 percent must never compare equal.
  EXPECT_NE(ByteSize::bytes(100), pct(100));
}

TEST(ByteSizeRoundTrip, ToStringOutputParsesBackIdentically) {
  for (const ByteSize original :
       {ByteSize::bytes(0), ByteSize::bytes(1), ByteSize::bytes(1536), ByteSize::bytes(4 * kKiB),
        ByteSize::bytes(8 * kGiB), pct(1), pct(70), pct(100)}) {
    const std::string text = to_string(original);
    const auto reparsed = parse_byte_size(text);
    ASSERT_TRUE(reparsed.has_value()) << "round trip failed for '" << text << "'";
    EXPECT_EQ(reparsed.value(), original) << "via '" << text << "'";
  }
}

}  // namespace
}  // namespace loadforge::core
