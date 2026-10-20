// SPDX-License-Identifier: GPL-3.0-or-later
//
// The strict LZ4 block decoder, rule by rule and boundary by boundary, on
// blocks written by hand from the block-format document. The reference
// tool's own blocks, and its decodings of blocks like these, are the T3
// tier's (tests/oracle/verification/oracle/compression/).

#include "verification/oracle/compression/decoder.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "unit/verification/oracle/compression/lz4_support.hpp"

namespace loadforge::verification::oracle::compression {
namespace {

using test::lz4::block_of;
using test::lz4::Bytes;
using test::lz4::encode;
using test::lz4::Sequence;
using test::lz4::slice;
using test::lz4::text;

struct Decoding {
  core::Result<Summary, Malformation> result;
  Bytes decoded;
};

Decoding decode(const Bytes& block, std::size_t size) {
  Bytes decoded(size, std::byte{0xEE});
  auto result = decode_block(block, decoded);
  return Decoding{result, decoded};
}

Bytes bytes(std::initializer_list<unsigned> values) {
  Bytes out;
  for (const unsigned value : values) {
    out.push_back(static_cast<std::byte>(value));
  }
  return out;
}

Bytes letters(std::size_t count) {
  Bytes out(count);
  for (std::size_t at = 0; at < count; ++at) {
    out[at] = static_cast<std::byte>('A' + at % 26);
  }
  return out;
}

void expect_malformed(const Bytes& block, std::size_t size, Rule rule, std::size_t at,
                      std::size_t decoded) {
  const Decoding decoding = decode(block, size);
  ASSERT_FALSE(decoding.result.has_value()) << describe(rule);
  EXPECT_EQ(decoding.result.error().rule, rule) << describe(decoding.result.error().rule);
  EXPECT_EQ(decoding.result.error().at, at) << describe(rule);
  EXPECT_EQ(decoding.result.error().decoded, decoded) << describe(rule);
}

Summary expect_decodes(const Bytes& block, const Bytes& expected) {
  const Decoding decoding = decode(block, expected.size());
  EXPECT_TRUE(decoding.result.has_value())
      << describe(decoding.result.error().rule) << " at " << decoding.result.error().at;
  EXPECT_EQ(decoding.decoded, expected);
  return decoding.result.has_value() ? decoding.result.value() : Summary{};
}

// The document's three examples of a literal length, byte for byte, and each
// decoded back.
TEST(Lz4Decoder, WritesAndReadsTheDocumentsLiteralLengthExamples) {
  EXPECT_EQ(slice(encode({letters(48)}), 0, 2), bytes({0xF0, 33}));
  EXPECT_EQ(slice(encode({letters(280)}), 0, 3), bytes({0xF0, 255, 10}));
  EXPECT_EQ(slice(encode({letters(15)}), 0, 2), bytes({0xF0, 0}));
  for (const std::size_t length : {48U, 280U, 15U}) {
    expect_decodes(encode({letters(length)}), letters(length));
  }
}

TEST(Lz4Decoder, EmptyDataIsOneZeroToken) {
  const Summary summary = expect_decodes(bytes({0x00}), {});
  EXPECT_EQ(summary.sequences, 1U);
  EXPECT_EQ(summary.matches, 0U);
}

TEST(Lz4Decoder, LiteralLengthsEitherSideOfEachMark) {
  // 14 fits the token; 15 is the token's mark and a zero byte; 15 + 255 and
  // 15 + 2 * 255 end their 255s with a zero byte; one either side of each.
  for (const std::size_t length : {0U, 1U, 14U, 15U, 16U, 269U, 270U, 271U, 524U, 525U, 526U}) {
    const Summary summary = expect_decodes(encode({letters(length)}), letters(length));
    EXPECT_EQ(summary.longest_literals, length);
  }
  EXPECT_EQ(encode({letters(270)}).size(), 1U + 2U + 270U);
  EXPECT_EQ(slice(encode({letters(525)}), 0, 4), bytes({0xF0, 255, 255, 0}));
}

TEST(Lz4Decoder, MatchLengthsEitherSideOfEachMark) {
  for (const std::size_t length : {4U, 5U, 18U, 19U, 20U, 273U, 274U, 275U, 528U, 529U}) {
    const Bytes tail = letters(12);
    const Bytes block = block_of({{text("abcd"), 4, length}, {tail}});
    Bytes expected = text("abcd");
    for (std::size_t at = 0; at < length; ++at) {
      expected.push_back(expected[at]);
    }
    expected.insert(expected.end(), tail.begin(), tail.end());
    const Summary summary = expect_decodes(block, expected);
    EXPECT_EQ(summary.longest_match, length);
    EXPECT_EQ(summary.matches, 1U);
  }
  EXPECT_EQ(encode({text("abcd"), 4, 18}).front(), std::byte{0x4E});
  EXPECT_EQ(slice(encode({text("abcd"), 4, 19}), 0, 1), bytes({0x4F}));
  EXPECT_EQ(slice(encode({text("abcd"), 4, 274}), 5, 9), bytes({4, 0, 255, 0}));
}

TEST(Lz4Decoder, AnEmptyLiteralRunBeforeAMatch) {
  const Bytes block = block_of({{text("abcd"), 4, 4}, {{}, 8, 4}, {letters(8)}});
  const Bytes tail = letters(8);
  Bytes expected = text("abcdabcdabcd");
  expected.insert(expected.end(), tail.begin(), tail.end());
  const Summary summary = expect_decodes(block, expected);
  EXPECT_EQ(summary.sequences, 3U);
  EXPECT_EQ(summary.matches, 2U);
  EXPECT_EQ(summary.max_offset, 8U);
}

TEST(Lz4Decoder, AMatchOverlappingItsOwnOutputRepeatsWhatItWrites) {
  // Offset 1: the last byte repeated. Offset 3 over 30 bytes: the period-3
  // run the document's "overlap match" describes.
  const Bytes tail = letters(12);
  Bytes ones(41, static_cast<std::byte>('z'));
  ones.insert(ones.end(), tail.begin(), tail.end());
  Summary summary = expect_decodes(block_of({{text("z"), 1, 40}, {tail}}), ones);
  EXPECT_EQ(summary.max_offset, 1U);
  EXPECT_EQ(summary.overlapping_matches, 1U);

  Bytes period;
  for (std::size_t at = 0; at < 33; ++at) {
    period.push_back(static_cast<std::byte>("xyz"[at % 3]));
  }
  period.insert(period.end(), tail.begin(), tail.end());
  summary = expect_decodes(block_of({{text("xyz"), 3, 30}, {tail}}), period);
  EXPECT_EQ(summary.overlapping_matches, 1U);

  // A match no longer than its offset does not overlap.
  Bytes apart = text("abcdabcd");
  apart.insert(apart.end(), tail.begin(), tail.end());
  summary = expect_decodes(block_of({{text("abcd"), 4, 4}, {tail}}), apart);
  EXPECT_EQ(summary.overlapping_matches, 0U);
}

TEST(Lz4Decoder, TheLargestOffsetReachesTheFirstByteAndNoFurther) {
  constexpr std::size_t kFar = 65535;
  const Bytes head = letters(kFar);
  const Bytes tail = letters(12);
  Bytes expected = head;
  expected.insert(expected.end(), head.begin(), head.begin() + 4);
  expected.insert(expected.end(), tail.begin(), tail.end());
  const Summary summary = expect_decodes(block_of({{head, kFar, 4}, {tail}}), expected);
  EXPECT_EQ(summary.max_offset, kFar);

  // One literal fewer and the same offset reaches before the start.
  const Bytes short_head = letters(kFar - 1);
  const Bytes block = block_of({{short_head, kFar, 4}, {tail}});
  const std::size_t offset_at = 1 + 1 + (kFar - 1 - 15) / 255 + (kFar - 1);
  expect_malformed(block, expected.size() - 1, Rule::kOffsetBeforeStart, offset_at, kFar - 1);
}

TEST(Lz4Decoder, AnOffsetEqualToWhatIsDecodedReachesTheFirstByte) {
  const Bytes tail = letters(12);
  Bytes expected = text("abcdabcd");
  expected.insert(expected.end(), tail.begin(), tail.end());
  expect_decodes(block_of({{text("abcd"), 4, 4}, {tail}}), expected);
  expect_malformed(block_of({{text("abcd"), 5, 4}, {tail}}), expected.size(),
                   Rule::kOffsetBeforeStart, 5, 4);
}

// "The last match must start at least 12 bytes before the end of block" and
// "the last 5 bytes of input are always literals", each met exactly and
// missed by one.
TEST(Lz4Decoder, TheEndOfBlockMarginsMetExactlyAndMissedByOne) {
  // Seven literals, a match of seven at offset 7, five literals: the match
  // starts exactly 12 bytes before the end and ends exactly 5 before it.
  const Bytes exact = block_of({{text("abcdefg"), 7, 7}, {text("VWXYZ")}});
  expect_decodes(exact, text("abcdefgabcdefgVWXYZ"));

  // Starts 11 bytes before the end.
  const Bytes late = block_of({{text("abcdefg"), 7, 6}, {text("VWXYZ")}});
  expect_malformed(late, 18, Rule::kMatchTooLate, 8, 7);

  // Ends 4 bytes before the end.
  const Bytes into = block_of({{text("abcdefg"), 7, 8}, {text("VWXY")}});
  expect_malformed(into, 19, Rule::kMatchIntoLastLiterals, 8, 7);

  // Data shorter than 12 bytes cannot hold a match at all.
  expect_malformed(block_of({{text("a"), 1, 4}, {text("bcdef")}}), 10, Rule::kMatchTooLate, 2, 1);
}

TEST(Lz4Decoder, AMatchThatWouldDecodePastTheEndIsIntoTheLastLiterals) {
  const Bytes block = block_of({{text("abcd"), 4, 100}, {letters(5)}});
  expect_malformed(block, 20, Rule::kMatchIntoLastLiterals, 5, 4);
}

TEST(Lz4Decoder, TheBlockMustEndWithASequenceOfLiteralsOnly) {
  // Empty: no sequence at all.
  expect_malformed({}, 0, Rule::kNoLastSequence, 0, 0);
  // Ends after a match.
  const Bytes block = encode({text("abcdefgh"), 8, 4});
  expect_malformed(block, 32, Rule::kNoLastSequence, block.size(), 12);
}

TEST(Lz4Decoder, TheLastTokenCarriesNoMatchLength) {
  for (unsigned field = 1; field <= 15; ++field) {
    expect_malformed(encode({text("hello"), std::nullopt, 0, field}), 5, Rule::kLastTokenHasMatch,
                     0, 5);
  }
}

TEST(Lz4Decoder, TheBlockMustDecodeToExactlyTheSize) {
  expect_malformed(encode({text("abcd")}), 5, Rule::kDecodedTooShort, 0, 4);
  expect_malformed(encode({text("abcdef")}), 5, Rule::kLiteralsTooLong, 1, 0);
  // Too long after a match: the last literals overrun.
  const Bytes block = block_of({{text("abcd"), 4, 4}, {letters(13)}});
  expect_malformed(block, 20, Rule::kLiteralsTooLong, 8, 8);
}

TEST(Lz4Decoder, ABlockThatEndsInsideAFieldSaysWhichAndWhere) {
  // Inside the literal length: no extension byte, then a 255 and nothing.
  expect_malformed(bytes({0xF0}), 100, Rule::kLiteralLengthTruncated, 1, 0);
  expect_malformed(bytes({0xF0, 0xFF}), 400, Rule::kLiteralLengthTruncated, 2, 0);
  // Inside the literals.
  expect_malformed(bytes({0x30, 'a', 'b'}), 3, Rule::kLiteralsTruncated, 1, 0);
  // Inside an offset: one byte of its two.
  expect_malformed(bytes({0x40, 'a', 'b', 'c', 'd', 0x04}), 20, Rule::kOffsetTruncated, 5, 4);
  // Inside a match length: no extension byte, then a 255 and nothing.
  expect_malformed(bytes({0x4F, 'a', 'b', 'c', 'd', 0x04, 0x00}), 40, Rule::kMatchLengthTruncated,
                   7, 4);
  expect_malformed(bytes({0x4F, 'a', 'b', 'c', 'd', 0x04, 0x00, 0xFF}), 400,
                   Rule::kMatchLengthTruncated, 8, 4);
}

TEST(Lz4Decoder, AnOffsetOfZeroIsInvalid) {
  const Bytes block = block_of({{text("abcd"), 0, 4}, {letters(12)}});
  expect_malformed(block, 20, Rule::kOffsetZero, 5, 4);
}

TEST(Lz4Decoder, TheBytesBeforeAMalformationAreDecoded) {
  const Bytes block = block_of({{text("abcd"), 4, 4}, {text("efgh"), 0, 4}, {letters(12)}});
  const Decoding decoding = decode(block, 40);
  ASSERT_FALSE(decoding.result.has_value());
  EXPECT_EQ(decoding.result.error().rule, Rule::kOffsetZero);
  EXPECT_EQ(decoding.result.error().decoded, 12U);
  EXPECT_EQ(slice(decoding.decoded, 0, 12), text("abcdabcdefgh"));
  EXPECT_EQ(decoding.decoded[12], std::byte{0xEE});
}

TEST(Lz4Decoder, EveryRuleHasItsOwnDescription) {
  std::set<std::string> seen;
  for (unsigned value = 0; value <= static_cast<unsigned>(Rule::kMatchIntoLastLiterals); ++value) {
    const std::string text(describe(static_cast<Rule>(value)));
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text, "unrecognised rule");
    EXPECT_TRUE(seen.insert(text).second) << text;
  }
  EXPECT_EQ(seen.size(), 12U);
  EXPECT_EQ(describe(static_cast<Rule>(12)), "unrecognised rule");
}

}  // namespace
}  // namespace loadforge::verification::oracle::compression
