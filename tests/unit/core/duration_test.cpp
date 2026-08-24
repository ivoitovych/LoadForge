// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/duration.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace loadforge::core {
namespace {

Duration parsed(std::string_view text) {
  const auto r = parse_duration(text);
  EXPECT_TRUE(r.has_value()) << "unexpectedly rejected: '" << text << "'";
  return r.value_or(Duration{-1});
}

ParseError rejected(std::string_view text) {
  const auto r = parse_duration(text);
  EXPECT_FALSE(r.has_value()) << "unexpectedly accepted: '" << text << "'";
  return r.has_value() ? ParseError::Empty : r.error();
}

TEST(ParseDuration, AcceptsEveryUnit) {
  EXPECT_EQ(parsed("500ms"), Duration{500});
  EXPECT_EQ(parsed("30s"), Duration{30 * 1000});
  EXPECT_EQ(parsed("20m"), Duration{20 * 60 * 1000});
  EXPECT_EQ(parsed("5h"), Duration{5 * 60 * 60 * 1000});
}

TEST(ParseDuration, MatchesLongestSuffixFirst) {
  // "ms" must not be read as "m" followed by junk.
  EXPECT_EQ(parsed("1ms"), Duration{1});
  EXPECT_EQ(parsed("1m"), Duration{60 * 1000});
}

TEST(ParseDuration, AcceptsZero) {
  EXPECT_EQ(parsed("0s"), Duration{0});
  EXPECT_EQ(parsed("0ms"), Duration{0});
}

TEST(ParseDuration, ToleratesSurroundingWhitespace) {
  EXPECT_EQ(parsed("  20m"), Duration{20 * 60 * 1000});
  EXPECT_EQ(parsed("20m  "), Duration{20 * 60 * 1000});
  EXPECT_EQ(parsed("\t 20m \t"), Duration{20 * 60 * 1000});
}

TEST(ParseDuration, RejectsEmptyAndWhitespaceOnly) {
  EXPECT_EQ(rejected(""), ParseError::Empty);
  EXPECT_EQ(rejected("   "), ParseError::Empty);
  EXPECT_EQ(rejected("\t"), ParseError::Empty);
}

TEST(ParseDuration, RejectsNegative) {
  EXPECT_EQ(rejected("-5m"), ParseError::NegativeValue);
  EXPECT_EQ(rejected("  -1s"), ParseError::NegativeValue);
}

TEST(ParseDuration, RejectsMalformedNumber) {
  EXPECT_EQ(rejected("abc"), ParseError::InvalidNumber);
  EXPECT_EQ(rejected("m"), ParseError::InvalidNumber);
  EXPECT_EQ(rejected("+5s"), ParseError::InvalidNumber);
  EXPECT_EQ(rejected(".5s"), ParseError::InvalidNumber);
}

TEST(ParseDuration, RejectsBareNumber) {
  // A unit is mandatory: "20" could plausibly mean seconds or minutes, and
  // guessing wrong silently changes a five-hour run into a five-minute one.
  EXPECT_EQ(rejected("20"), ParseError::MissingUnit);
  EXPECT_EQ(rejected("0"), ParseError::MissingUnit);
}

TEST(ParseDuration, RejectsUnknownUnit) {
  EXPECT_EQ(rejected("20x"), ParseError::UnknownUnit);
  EXPECT_EQ(rejected("20 m"), ParseError::UnknownUnit);  // inner space is not a unit
  EXPECT_EQ(rejected("5d"), ParseError::UnknownUnit);
  EXPECT_EQ(rejected("1sec"), ParseError::UnknownUnit);
}

TEST(ParseDuration, RejectsCompoundForms) { EXPECT_EQ(rejected("1h30m"), ParseError::UnknownUnit); }

TEST(ParseDuration, RejectsNumberTooLargeForUint64) {
  EXPECT_EQ(rejected("99999999999999999999999s"), ParseError::OutOfRange);
}

TEST(ParseDuration, RejectsValueThatOverflowsWhenScaled) {
  // Fits in uint64 but not once multiplied into milliseconds.
  EXPECT_EQ(rejected("2562047788016h"), ParseError::OutOfRange);
  EXPECT_EQ(rejected("9223372036854776s"), ParseError::OutOfRange);
}

TEST(ParseDuration, AcceptsLargestValueThatStillFits) {
  EXPECT_EQ(parsed("2562047788015h").count(), 2562047788015LL * 60 * 60 * 1000);
}

TEST(ParseDuration, ValueOrYieldsFallbackForRejectedInput) {
  // The helpers above only reach value_or on the success path; the fallback
  // path for this instantiation needs exercising too.
  EXPECT_EQ(parse_duration("nonsense").value_or(Duration{7}), Duration{7});
  EXPECT_EQ(parse_duration("20m").value_or(Duration{7}), Duration{20 * 60 * 1000});
}

TEST(DurationToString, ChoosesLargestExactUnit) {
  EXPECT_EQ(to_string(Duration{5 * 60 * 60 * 1000}), "5h");
  EXPECT_EQ(to_string(Duration{20 * 60 * 1000}), "20m");
  EXPECT_EQ(to_string(Duration{30 * 1000}), "30s");
  EXPECT_EQ(to_string(Duration{500}), "500ms");
}

TEST(DurationToString, FallsBackToMillisForZeroAndIndivisible) {
  EXPECT_EQ(to_string(Duration{0}), "0ms");
  EXPECT_EQ(to_string(Duration{1500}), "1500ms");  // 1.5s is not exact in any larger unit
}

TEST(DurationToString, RejectsNegativeRatherThanEmittingAHugePositive) {
  // Duration is an alias for chrono::milliseconds so it interoperates with
  // chrono, which makes Duration{-1} constructible. Casting that to an unsigned
  // type would render something like "18446744073709551615ms" -- text the
  // parser then rejects. A silent wrong answer is the one output this project
  // must never produce.
  EXPECT_THROW((void)to_string(Duration{-1}), std::invalid_argument);
  EXPECT_THROW((void)to_string(Duration{-60000}), std::invalid_argument);
  EXPECT_NO_THROW((void)to_string(Duration{0}));
}

TEST(DurationRoundTrip, ToStringOutputParsesBackIdentically) {
  for (const Duration original :
       {Duration{0}, Duration{1}, Duration{999}, Duration{1000}, Duration{90 * 1000},
        Duration{20 * 60 * 1000}, Duration{5 * 60 * 60 * 1000}}) {
    const std::string text = to_string(original);
    const auto reparsed = parse_duration(text);
    ASSERT_TRUE(reparsed.has_value()) << "round trip failed for '" << text << "'";
    EXPECT_EQ(reparsed.value(), original) << "via '" << text << "'";
  }
}

}  // namespace
}  // namespace loadforge::core
