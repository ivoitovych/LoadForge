// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the compression workload: a corrupted output is caught by the test
// decoder, and each test says how -- which check fired and at which byte.
// First the decoder's own breaches, one hand-built block each, so that every
// way a block can be wrong is shown to be told apart from the others; then
// corruptions of a real unit's output, at the header, the block, the padding
// and the input it is judged against; then every single-bit flip over the
// parts of an output where the decoder's rules are densest.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "oracle/workloads/compression/lz4_decoder.hpp"
#include "unit/workloads/compression/support.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/compression/generator.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Breach;
using testing::Bytes;
using testing::bytes_of;
using testing::Check;
using testing::concat;
using testing::decode_block;
using testing::Domain;
using testing::judge_unit;
using testing::Judgement;
using testing::Verdict;

void expect_breach(const Bytes& block, std::size_t capacity, Breach breach, std::size_t at) {
  const testing::Decoded decoded = decode_block(block, capacity);
  EXPECT_EQ(decoded.breach, breach)
      << describe(decoded.breach) << ", expected " << describe(breach);
  EXPECT_EQ(decoded.at, at) << describe(breach);
}

TEST(Lz4DecoderBreach, AnEmptyBlockHasNoToken) { expect_breach({}, 16, Breach::kNoToken, 0); }

TEST(Lz4DecoderBreach, ALengthThatPromisesMoreBytesThanTheBlockHas) {
  expect_breach(bytes_of({0xF0}), 64, Breach::kTruncatedLength, 1);
  expect_breach(bytes_of({0xF0, 0xFF}), 512, Breach::kTruncatedLength, 2);
  // The match length's escape, at the end.
  expect_breach(bytes_of({0x1F, 'a', 0x01, 0x00}), 64, Breach::kTruncatedLength, 4);
}

TEST(Lz4DecoderBreach, LiteralsRunningPastTheBlock) {
  expect_breach(bytes_of({0x30, 'a', 'b'}), 64, Breach::kTruncatedLiterals, 1);
  expect_breach(bytes_of({0xF0, 0xFF, 0x00, 'a'}), 4096, Breach::kTruncatedLiterals, 3);
}

TEST(Lz4DecoderBreach, AnOffsetCutShort) {
  expect_breach(bytes_of({0x10, 'a', 0x01}), 64, Breach::kTruncatedOffset, 2);
}

TEST(Lz4DecoderBreach, OffsetZeroIsInvalid) {
  expect_breach(concat({bytes_of({0x10, 'a', 0x00, 0x00, 0x50}), bytes_of("aaaaa")}), 64,
                Breach::kOffsetZero, 2);
}

TEST(Lz4DecoderBreach, AnOffsetReachingBeforeTheFirstByte) {
  expect_breach(concat({bytes_of({0x10, 'a', 0x02, 0x00, 0x50}), bytes_of("aaaaa")}), 64,
                Breach::kOffsetBeforeStart, 2);
  // Nothing decoded yet, so even offset 1 is before the start.
  expect_breach(bytes_of({0x00, 0x01, 0x00}), 64, Breach::kOffsetBeforeStart, 1);
  const testing::Decoded exact =
      decode_block(concat({bytes_of({0x20, 'a', 'b', 0x02, 0x00, 0x50}), bytes_of("ababa")}), 64);
  EXPECT_EQ(exact.breach, Breach::kLateMatch) << "an offset of exactly what is decoded is valid";
}

TEST(Lz4DecoderBreach, DecodingMoreThanTheCapacity) {
  expect_breach(bytes_of({0x20, 'a', 'b'}), 1, Breach::kPastCapacity, 1);
  // The match's four bytes do not fit after one literal.
  expect_breach(concat({bytes_of({0x10, 'a', 0x01, 0x00, 0x50}), bytes_of("aaaaa")}), 4,
                Breach::kPastCapacity, 4);
}

TEST(Lz4DecoderBreach, ABlockThatEndsWithAMatch) {
  expect_breach(bytes_of({0x10, 'a', 0x01, 0x00}), 64, Breach::kEndsWithMatch, 4);
}

TEST(Lz4DecoderBreach, FewerThanFiveLastLiterals) {
  // 13 bytes with a match starting twelve before the end, and only four last
  // literals: the match is early enough, the literals too few.
  expect_breach(concat({bytes_of({0x14, 'a', 0x01, 0x00, 0x40}), bytes_of("aaaa")}), 64,
                Breach::kShortLastLiterals, 4);
  const testing::Decoded short_input = decode_block(bytes_of({0x40, 'a', 'b', 'c', 'd'}), 64);
  EXPECT_EQ(short_input.breach, Breach::kNone) << "four bytes in all may be four literals";
}

TEST(Lz4DecoderBreach, AMatchStartingFewerThanTwelveBytesBeforeTheEnd) {
  // Ten bytes: the match starts at 1, nine before the end.
  expect_breach(concat({bytes_of({0x10, 'a', 0x01, 0x00, 0x50}), bytes_of("aaaaa")}), 64,
                Breach::kLateMatch, 4);
  const testing::Decoded allowed =
      decode_block(concat({bytes_of({0x13, 'a', 0x01, 0x00, 0x50}), bytes_of("aaaaa")}), 64);
  EXPECT_EQ(allowed.breach, Breach::kNone) << "thirteen bytes: exactly twelve before the end";
}

TEST(Lz4DecoderBreach, EveryBreachHasItsOwnName) {
  std::vector<std::string> names;
  for (int value = 0; value <= static_cast<int>(Breach::kLateMatch); ++value) {
    names.emplace_back(describe(static_cast<Breach>(value)));
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      EXPECT_NE(names[i], names[j]);
    }
  }
  EXPECT_EQ(describe(static_cast<Breach>(99)), "unrecognised breach");
}

/// A real unit: seed 1, index 0, the input the real tool's decode of this
/// compressor's block matched (tests/fixtures/lz4-workload/).
struct Unit {
  Bytes input;
  Bytes output;
  std::size_t length = 0;
};

Unit unit() {
  Unit made;
  made.input.resize(kInputBytes);
  generate_input(1, 0, made.input);
  made.output.resize(output_bytes_for(kInputBytes));
  Bytes table(kTableBytes);
  write_unit(made.input, made.output, table);
  made.length = load_le32(made.output, 4);
  return made;
}

void store_le32(Bytes& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t k = 0; k < 4; ++k) {
    bytes[at + k] = static_cast<std::byte>(value >> (8 * k));
  }
}

void expect_judged(const Judgement& judgement, Check check, std::size_t at,
                   Domain domain = Domain::kStored, Breach breach = Breach::kNone) {
  EXPECT_EQ(judgement.verdict, Verdict::kMismatch);
  EXPECT_EQ(judgement.check, check);
  EXPECT_EQ(judgement.at, at);
  EXPECT_EQ(judgement.domain, domain);
  EXPECT_EQ(judgement.breach, breach) << describe(judgement.breach);
}

TEST(CompressionCorruption, TheUncorruptedUnitAgrees) {
  const Unit clean = unit();
  const Judgement judgement = judge_unit(clean.input, clean.output);
  EXPECT_EQ(judgement.verdict, Verdict::kAgreement);
  EXPECT_LT(clean.length, kInputBytes) << "the unit compresses, so there is padding to corrupt";
}

TEST(CompressionCorruption, AnyBitOfTheMagicIsCaughtAsTheMagic) {
  for (std::size_t bit = 0; bit < 32; ++bit) {
    Unit bad = unit();
    bad.output[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    expect_judged(judge_unit(bad.input, bad.output), Check::kMagic, 0);
  }
}

TEST(CompressionCorruption, ALengthOverTheBoundIsCaughtAsSuch) {
  for (const std::uint32_t length :
       {static_cast<std::uint32_t>(compress_bound(kInputBytes) + 1), 0x80000000U, 0xFFFFFFFFU}) {
    Unit bad = unit();
    store_le32(bad.output, 4, length);
    expect_judged(judge_unit(bad.input, bad.output), Check::kLengthOverBound, 4);
  }
  Unit at_bound = unit();
  store_le32(at_bound.output, 4, static_cast<std::uint32_t>(compress_bound(kInputBytes)));
  EXPECT_NE(judge_unit(at_bound.input, at_bound.output).check, Check::kLengthOverBound)
      << "the bound itself is a length the format allows";
}

TEST(CompressionCorruption, ALengthOneShortLeavesTheLastLiteralAsPadding) {
  Unit bad = unit();
  ASSERT_NE(bad.input.back(), std::byte{0}) << "the unit's last byte is not zero";
  store_le32(bad.output, 4, static_cast<std::uint32_t>(bad.length - 1));
  expect_judged(judge_unit(bad.input, bad.output), Check::kPaddingNotZero, 8 + bad.length - 1);
}

TEST(CompressionCorruption, ALengthOneLongReadsAZeroPaddingByteAsAnOffset) {
  Unit bad = unit();
  store_le32(bad.output, 4, static_cast<std::uint32_t>(bad.length + 1));
  expect_judged(judge_unit(bad.input, bad.output), Check::kBlock, 8 + bad.length, Domain::kStored,
                Breach::kTruncatedOffset);
  store_le32(bad.output, 4, static_cast<std::uint32_t>(bad.length + 2));
  expect_judged(judge_unit(bad.input, bad.output), Check::kBlock, 8 + bad.length, Domain::kStored,
                Breach::kOffsetZero);
}

TEST(CompressionCorruption, ANonZeroPaddingByteIsCaughtWhereItIs) {
  for (const std::size_t from_block_end : {std::size_t{0}, std::size_t{100}}) {
    Unit bad = unit();
    const std::size_t at = 8 + bad.length + from_block_end;
    bad.output[at] = std::byte{0x01};
    expect_judged(judge_unit(bad.input, bad.output), Check::kPaddingNotZero, at);
  }
  Unit last = unit();
  last.output.back() = std::byte{0x80};
  expect_judged(judge_unit(last.input, last.output), Check::kPaddingNotZero,
                last.output.size() - 1);
}

TEST(CompressionCorruption, AFlippedLiteralIsCaughtAsTheDecodedByteItBecomes) {
  Unit first = unit();
  const auto token = std::to_integer<unsigned>(first.output[8]);
  ASSERT_GT(token >> 4U, 0U);
  ASSERT_LT(token >> 4U, 15U) << "so the first literal follows the token directly";
  first.output[9] ^= std::byte{0x01};
  expect_judged(judge_unit(first.input, first.output), Check::kDecodedBytes, 0, Domain::kDerived);

  Unit last = unit();
  last.output[8 + last.length - 1] ^= std::byte{0x40};
  expect_judged(judge_unit(last.input, last.output), Check::kDecodedBytes, kInputBytes - 1,
                Domain::kDerived);
}

/// The block position of the first match's offset, and where its copy starts
/// in the decoded bytes, read from the clean block by the test decoder.
struct FirstMatch {
  std::size_t offset_at = 0;
  std::size_t decoded_at = 0;
  std::size_t offset = 0;
};

FirstMatch first_match(const Unit& clean) {
  const testing::Decoded decoded =
      decode_block(std::span<const std::byte>(clean.output).subspan(8, clean.length), kInputBytes);
  const testing::Sequence first = decoded.sequences.at(0);
  std::size_t at = 1 + first.literals;
  if (first.literals >= 15) {
    at += 1 + (first.literals - 15) / 255;
  }
  return FirstMatch{8 + at, first.literals, first.offset};
}

TEST(CompressionCorruption, AnOffsetMadeZeroOrTooFarIsCaughtAtTheOffset) {
  const FirstMatch match = first_match(unit());
  Unit zero = unit();
  zero.output[match.offset_at] = std::byte{0};
  zero.output[match.offset_at + 1] = std::byte{0};
  expect_judged(judge_unit(zero.input, zero.output), Check::kBlock, match.offset_at,
                Domain::kStored, Breach::kOffsetZero);

  Unit far = unit();
  const std::size_t beyond = match.decoded_at + 1;
  far.output[match.offset_at] = static_cast<std::byte>(beyond & 0xFFU);
  far.output[match.offset_at + 1] = static_cast<std::byte>(beyond >> 8U);
  expect_judged(judge_unit(far.input, far.output), Check::kBlock, match.offset_at, Domain::kStored,
                Breach::kOffsetBeforeStart);
}

TEST(CompressionCorruption, AnOffsetMovedByOneCopiesTheWrongBytes) {
  const Unit clean = unit();
  const FirstMatch match = first_match(clean);
  Unit bad = unit();
  const std::size_t moved = match.offset - 1;
  ASSERT_GT(moved, 0U);
  bad.output[match.offset_at] = static_cast<std::byte>(moved & 0xFFU);
  bad.output[match.offset_at + 1] = static_cast<std::byte>(moved >> 8U);
  const Judgement judgement = judge_unit(bad.input, bad.output);
  EXPECT_EQ(judgement.check, Check::kDecodedBytes);
  EXPECT_EQ(judgement.domain, Domain::kDerived);
  EXPECT_GE(judgement.at, match.decoded_at) << "nothing before the match changed";
}

TEST(CompressionCorruption, ABlockCutAfterAMatchEndsWithTheMatch) {
  const FirstMatch match = first_match(unit());
  Unit bad = unit();
  const std::size_t cut = match.offset_at + 2 - 8;  // just after the offset
  const auto token = std::to_integer<unsigned>(bad.output[8]);
  ASSERT_LT(token & 0x0FU, 15U) << "the first match has no extra length bytes";
  store_le32(bad.output, 4, static_cast<std::uint32_t>(cut));
  std::fill(bad.output.begin() + static_cast<std::ptrdiff_t>(8 + cut), bad.output.end(),
            std::byte{0});
  expect_judged(judge_unit(bad.input, bad.output), Check::kBlock, 8 + cut, Domain::kStored,
                Breach::kEndsWithMatch);
}

TEST(CompressionCorruption, AValidBlockOfTheWrongSizeIsCaughtBySizeOrCapacity) {
  Unit shorter = unit();
  const Bytes block = testing::compressed(std::span<const std::byte>(shorter.input).first(1000));
  std::fill(shorter.output.begin() + 4, shorter.output.end(), std::byte{0});
  store_le32(shorter.output, 4, static_cast<std::uint32_t>(block.size()));
  std::ranges::copy(block, shorter.output.begin() + 8);
  expect_judged(judge_unit(shorter.input, shorter.output), Check::kDecodedSize, 1000,
                Domain::kDerived);

  Unit longer = unit();
  const Bytes zeros(kInputBytes + 1, std::byte{0});
  const Bytes too_long = testing::compressed(zeros);
  std::fill(longer.output.begin() + 4, longer.output.end(), std::byte{0});
  store_le32(longer.output, 4, static_cast<std::uint32_t>(too_long.size()));
  std::ranges::copy(too_long, longer.output.begin() + 8);
  const Judgement judgement = judge_unit(longer.input, longer.output);
  EXPECT_EQ(judgement.check, Check::kBlock);
  EXPECT_EQ(judgement.breach, Breach::kPastCapacity);
}

TEST(CompressionCorruption, AnInputCorruptedAfterComputeIsCaughtAtTheCorruptedByte) {
  for (const std::size_t at : {std::size_t{0}, std::size_t{12345}, kInputBytes - 1}) {
    Unit bad = unit();
    bad.input[at] ^= std::byte{0x10};
    expect_judged(judge_unit(bad.input, bad.output), Check::kDecodedBytes, at, Domain::kDerived);
  }
}

TEST(CompressionCorruption, EverySingleBitFlipOfTheHeaderAndTheBlocksEndsIsCaught) {
  const Unit clean = unit();
  std::vector<std::size_t> positions;
  for (std::size_t at = 0; at < 8 + 512; ++at) {
    positions.push_back(at);
  }
  for (std::size_t at = 8 + clean.length - 512; at < 8 + clean.length + 64; ++at) {
    positions.push_back(at);
  }
  std::size_t flips = 0;
  for (const std::size_t at : positions) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Bytes output = clean.output;
      output[at] ^= static_cast<std::byte>(1U << bit);
      const Judgement judgement = judge_unit(clean.input, output);
      ++flips;
      // A flip may turn one valid block into another that decodes to the
      // same input -- an offset to an identical earlier copy -- which is a
      // right answer, not a missed corruption. Anything else must be caught.
      if (judgement.verdict == Verdict::kAgreement) {
        const testing::Decoded decoded = decode_block(
            std::span<const std::byte>(output).subspan(8, load_le32(output, 4)), kInputBytes);
        EXPECT_EQ(decoded.bytes, clean.input) << "byte " << at << " bit " << bit;
      } else {
        EXPECT_EQ(judgement.verdict, Verdict::kMismatch) << "byte " << at << " bit " << bit;
      }
    }
  }
  EXPECT_EQ(flips, (8 + 512 + 512 + 64) * 8U);
}

}  // namespace
}  // namespace loadforge::workloads::compression
