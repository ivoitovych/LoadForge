// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exact comparison at its boundaries: nothing, one byte, a difference in the
// first byte and in the last, a window cut short by the end of the bytes, a
// popcount that counts beyond the window, and the record each outcome calls
// for.

#include "verification/checksum/compare.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/equality.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

Bytes random_bytes(std::size_t size, std::uint64_t stream = 0) {
  Bytes bytes(size);
  rng::fill_stream(0xC0FFEE, stream, bytes);
  return bytes;
}

Subject subject() {
  Subject subject;
  subject.provenance.iteration = 7;
  subject.side = Side::kInput;
  subject.check = "input against its regeneration";
  subject.line_bytes = 64;
  return subject;
}

TEST(CompareExact, EqualBytesMatchAtAnyLength) {
  for (const std::size_t size : {std::size_t{1}, std::size_t{8}, std::size_t{1} << 20U}) {
    const Bytes bytes = random_bytes(size);
    const Comparison comparison = compare_exact(bytes, bytes, Domain::kStored);
    EXPECT_EQ(comparison.outcome, ComparisonOutcome::kMatch) << size;
    EXPECT_EQ(comparison.expected_bytes, size);
    EXPECT_EQ(comparison.actual_bytes, size);
    EXPECT_FALSE(comparison.difference.has_value());
  }
}

TEST(CompareExact, NothingToCompareIsNotAMatch) {
  const Comparison comparison = compare_exact({}, {}, Domain::kStored);
  EXPECT_EQ(comparison.outcome, ComparisonOutcome::kNothingCompared);
  EXPECT_FALSE(comparison.difference.has_value());
}

TEST(CompareExact, DifferentLengthsAreAMismatchWithNoBitsToClassify) {
  const Bytes longer = random_bytes(10);
  const Bytes shorter(longer.begin(), longer.end() - 1);
  for (const auto& [expected, actual] :
       {std::pair{longer, shorter}, std::pair{shorter, longer}, std::pair{longer, Bytes{}}}) {
    const Comparison comparison = compare_exact(expected, actual, Domain::kDerived);
    EXPECT_EQ(comparison.outcome, ComparisonOutcome::kLengthMismatch);
    EXPECT_EQ(comparison.expected_bytes, expected.size());
    EXPECT_EQ(comparison.actual_bytes, actual.size());
    EXPECT_FALSE(comparison.difference.has_value()) << "a prefix compared is bits invented";
  }
}

TEST(CompareExact, OneByteOneBit) {
  const Bytes expected = {std::byte{0x00}};
  const Bytes actual = {std::byte{0x80}};
  const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
  ASSERT_EQ(comparison.outcome, ComparisonOutcome::kMismatch);
  const Difference& difference = *comparison.difference;
  EXPECT_EQ(difference, (Difference{Domain::kStored, 0, 0, 1, 0, expected, actual}));
}

TEST(CompareExact, TheWindowStartsAtTheFirstDifferingWordAndStopsAtTheEnd) {
  const Bytes expected = random_bytes(10);
  Bytes actual = expected;
  actual[9] ^= std::byte{0x03};
  const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
  ASSERT_TRUE(comparison.difference.has_value());
  const Difference& difference = *comparison.difference;
  EXPECT_EQ(difference.first_offset, 9U);
  EXPECT_EQ(difference.last_offset, 9U);
  EXPECT_EQ(difference.popcount, 2U);
  EXPECT_EQ(difference.window_offset, 8U);
  EXPECT_EQ(difference.expected, Bytes(expected.begin() + 8, expected.end()))
      << "two bytes: the window is cut short by the end of the compared bytes";
  EXPECT_EQ(difference.actual, Bytes(actual.begin() + 8, actual.end()));
  EXPECT_TRUE(is_valid(difference));
}

TEST(CompareExact, ThePopcountCountsTheWholeComparisonNotTheWindow) {
  const Bytes expected = random_bytes(1000);
  Bytes actual = expected;
  actual[100] ^= std::byte{0x01};
  actual[500] ^= std::byte{0x0F};
  actual[999] ^= std::byte{0xFF};
  const Comparison comparison = compare_exact(expected, actual, Domain::kDerived);
  ASSERT_TRUE(comparison.difference.has_value());
  const Difference& difference = *comparison.difference;
  EXPECT_EQ(difference.domain, Domain::kDerived);
  EXPECT_EQ(difference.first_offset, 100U);
  EXPECT_EQ(difference.last_offset, 999U);
  EXPECT_EQ(difference.popcount, 13U);
  EXPECT_EQ(difference.window_offset, 96U);
  EXPECT_EQ(difference.expected.size(), kMaxWindowBytes);
  EXPECT_EQ(difference.expected, Bytes(expected.begin() + 96, expected.begin() + 160));
  EXPECT_EQ(difference.actual, Bytes(actual.begin() + 96, actual.begin() + 160));
}

TEST(CompareExact, TheFirstByteAndAWindowOfExactlyTheMaximum) {
  const Bytes expected = random_bytes(64);
  Bytes actual = expected;
  actual[0] ^= std::byte{0x40};
  const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
  ASSERT_TRUE(comparison.difference.has_value());
  EXPECT_EQ(comparison.difference->window_offset, 0U);
  EXPECT_EQ(comparison.difference->expected, expected);
  EXPECT_EQ(comparison.difference->actual, actual);
}

TEST(CompareExact, OutcomeWords) {
  EXPECT_EQ(describe(ComparisonOutcome::kMatch), "match");
  EXPECT_EQ(describe(ComparisonOutcome::kMismatch), "mismatch");
  EXPECT_EQ(describe(ComparisonOutcome::kLengthMismatch), "length mismatch");
  EXPECT_EQ(describe(ComparisonOutcome::kNothingCompared), "nothing compared");
  EXPECT_EQ(describe(static_cast<ComparisonOutcome>(9)), "unrecognised comparison outcome");
}

TEST(CompareExact, EveryMemberOfAComparisonIsCompared) {
  const Bytes expected = random_bytes(16);
  Bytes actual = expected;
  actual[3] ^= std::byte{1};
  contracts::expect_memberwise_equality(
      compare_exact(expected, actual, Domain::kStored),
      {[](Comparison& c) { c.outcome = ComparisonOutcome::kMatch; },
       [](Comparison& c) { c.expected_bytes = 1; }, [](Comparison& c) { c.actual_bytes = 1; },
       [](Comparison& c) { c.difference->popcount = 9; }});
}

TEST(CompareToRecord, AMatchCallsForNoRecord) {
  const Bytes bytes = random_bytes(32);
  EXPECT_EQ(to_record(compare_exact(bytes, bytes, Domain::kStored), subject()), std::nullopt);
}

TEST(CompareToRecord, AMismatchIsAnExactCorruptionCarryingItsDifference) {
  const Bytes expected = random_bytes(32);
  Bytes actual = expected;
  actual[17] ^= std::byte{0x20};
  const Comparison comparison = compare_exact(expected, actual, Domain::kStored);
  const std::optional<ErrorRecord> record = to_record(comparison, subject());
  ASSERT_TRUE(record.has_value());
  const auto* exact = std::get_if<ExactBitCorruption>(&*record);
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->provenance, subject().provenance);
  EXPECT_EQ(exact->side, Side::kInput);
  EXPECT_EQ(exact->check, "input against its regeneration");
  EXPECT_EQ(exact->line_bytes, 64U);
  EXPECT_EQ(exact->difference, comparison.difference);
  EXPECT_TRUE(is_valid(*record));
}

TEST(CompareToRecord, ALengthMismatchIsAnExactCorruptionWithoutBits) {
  const Bytes expected = random_bytes(32);
  const Bytes actual(expected.begin(), expected.begin() + 31);
  const std::optional<ErrorRecord> record =
      to_record(compare_exact(expected, actual, Domain::kDerived), subject());
  ASSERT_TRUE(record.has_value());
  const auto* exact = std::get_if<ExactBitCorruption>(&*record);
  ASSERT_NE(exact, nullptr);
  EXPECT_FALSE(exact->difference.has_value());
  EXPECT_EQ(exact->check,
            "input against its regeneration -- expected 32 bytes, found 31: no byte-for-byte "
            "difference to classify");
  EXPECT_EQ(classify(*exact), std::nullopt);
  EXPECT_TRUE(is_valid(*record));
}

TEST(CompareToRecord, NothingComparedIsVerificationNotPerformed) {
  const std::optional<ErrorRecord> record =
      to_record(compare_exact({}, {}, Domain::kStored), subject());
  ASSERT_TRUE(record.has_value());
  const auto* unrun = std::get_if<VerificationNotPerformed>(&*record);
  ASSERT_NE(unrun, nullptr);
  EXPECT_EQ(unrun->what, "input against its regeneration");
  EXPECT_EQ(unrun->why, "there were no bytes to compare: both sides were empty");
  EXPECT_EQ(unrun->blocks_compared, 0U);
  EXPECT_EQ(unrun->provenance, subject().provenance);
  EXPECT_FALSE(is_wrong_result(*record));
}

TEST(CompareToRecord, TheLongestCheckASubjectMayCarryStillMakesAValidRecord) {
  Subject longest = subject();
  longest.check = std::string(kMaxCheckBytes, 'c');
  Comparison widest;
  widest.outcome = ComparisonOutcome::kLengthMismatch;
  widest.expected_bytes = std::numeric_limits<std::uint64_t>::max();
  widest.actual_bytes = std::numeric_limits<std::uint64_t>::max() - 1;
  const std::optional<ErrorRecord> record = to_record(widest, longest);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(is_valid(*record));
}

}  // namespace
}  // namespace loadforge::verification
