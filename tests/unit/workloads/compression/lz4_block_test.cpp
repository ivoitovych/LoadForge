// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compressor's encoding, byte by byte, at every boundary of the format:
// each length field either side of its escape value and of each 255 step,
// the offset's two bytes, the empty, one-byte and shortest compressible
// blocks, the end-of-block rules at their limits, and the 65 535-byte offset
// with the first that cannot be coded. The expected bytes are written from
// the block-format document's own examples, not from the compressor.

#include "workloads/compression/lz4_block.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "oracle/workloads/compression/lz4_decoder.hpp"
#include "unit/workloads/compression/support.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Bytes;
using testing::bytes_of;
using testing::compressed;
using testing::concat;
using testing::noise;
using testing::Sequence;

/// write_sequence() alone, from position 0 of a buffer with room to spare.
Bytes sequence(std::span<const std::byte> literals, std::size_t offset, std::size_t match) {
  Bytes block(literals.size() + 64, std::byte{0xEE});
  block.resize(write_sequence(block, 0, literals, offset, match));
  return block;
}

/// The bytes the format document says a literal-only sequence of `count`
/// literals starts with: its token and length bytes.
Bytes literal_head(std::initializer_list<unsigned> head, std::size_t count) {
  return concat({bytes_of(head), Bytes(count, std::byte{'x'})});
}

TEST(Lz4Sequence, LiteralLengthsEitherSideOfEachEscape) {
  const auto literals = [](std::size_t count) { return Bytes(count, std::byte{'x'}); };
  EXPECT_EQ(sequence({}, 0, 0), bytes_of({0x00}))
      << "the empty block: a final token with no literal and no match";
  EXPECT_EQ(sequence(literals(1), 0, 0), literal_head({0x10}, 1));
  EXPECT_EQ(sequence(literals(14), 0, 0), literal_head({0xE0}, 14));
  EXPECT_EQ(sequence(literals(15), 0, 0), literal_head({0xF0, 0}, 15))
      << "example 3: 15 is 15 then 0 -- 'yes, the zero must be output'";
  EXPECT_EQ(sequence(literals(16), 0, 0), literal_head({0xF0, 1}, 16));
  EXPECT_EQ(sequence(literals(48), 0, 0), literal_head({0xF0, 33}, 48)) << "example 1";
  EXPECT_EQ(sequence(literals(269), 0, 0), literal_head({0xF0, 254}, 269));
  EXPECT_EQ(sequence(literals(270), 0, 0), literal_head({0xF0, 255, 0}, 270));
  EXPECT_EQ(sequence(literals(280), 0, 0), literal_head({0xF0, 255, 10}, 280)) << "example 2";
  EXPECT_EQ(sequence(literals(524), 0, 0), literal_head({0xF0, 255, 254}, 524));
  EXPECT_EQ(sequence(literals(525), 0, 0), literal_head({0xF0, 255, 255, 0}, 525));
}

TEST(Lz4Sequence, MatchLengthsEitherSideOfEachEscape) {
  EXPECT_EQ(sequence({}, 1, 4), bytes_of({0x00, 0x01, 0x00})) << "minmatch is coded as 0";
  EXPECT_EQ(sequence({}, 1, 5), bytes_of({0x01, 0x01, 0x00}));
  EXPECT_EQ(sequence({}, 1, 18), bytes_of({0x0E, 0x01, 0x00}));
  EXPECT_EQ(sequence({}, 1, 19), bytes_of({0x0F, 0x01, 0x00, 0}));
  EXPECT_EQ(sequence({}, 1, 20), bytes_of({0x0F, 0x01, 0x00, 1}));
  EXPECT_EQ(sequence({}, 1, 273), bytes_of({0x0F, 0x01, 0x00, 254}));
  EXPECT_EQ(sequence({}, 1, 274), bytes_of({0x0F, 0x01, 0x00, 255, 0}));
  EXPECT_EQ(sequence({}, 1, 529), bytes_of({0x0F, 0x01, 0x00, 255, 255, 0}));
}

TEST(Lz4Sequence, TheOffsetIsTwoBytesLowFirst) {
  EXPECT_EQ(sequence({}, 255, 4), bytes_of({0x00, 0xFF, 0x00}));
  EXPECT_EQ(sequence({}, 256, 4), bytes_of({0x00, 0x00, 0x01}));
  EXPECT_EQ(sequence({}, 0x1234, 4), bytes_of({0x00, 0x34, 0x12}));
  EXPECT_EQ(sequence({}, kMaxOffset, 4), bytes_of({0x00, 0xFF, 0xFF}));
}

TEST(Lz4Sequence, BothFieldsEscapedInOneTokenAndTheLiteralsBetween) {
  const Bytes literals = bytes_of("fifteen-letters");
  ASSERT_EQ(literals.size(), 15U);
  EXPECT_EQ(sequence(literals, 15, 19),
            concat({bytes_of({0xFF, 0}), literals, bytes_of({15, 0, 0})}));
}

TEST(Lz4Sequence, ReturnsThePositionAfterItAndWritesNothingBeyond) {
  Bytes block(16, std::byte{0xEE});
  const Bytes literals = bytes_of("ab");
  EXPECT_EQ(write_sequence(block, 3, literals, 2, 4), 3U + 1U + 2U + 2U);
  EXPECT_EQ(block, concat({Bytes(3, std::byte{0xEE}), bytes_of({0x20, 'a', 'b', 2, 0}),
                           Bytes(8, std::byte{0xEE})}));
}

TEST(Lz4Hash, TheTopTwelveBitsOfKnuthsProduct) {
  EXPECT_EQ(hash_slot(0), 0U);
  EXPECT_EQ(hash_slot(1), 2654435761U >> 20U);
  EXPECT_EQ(hash_slot(0xFFFFFFFFU), (4294967296U - 2654435761U) >> 20U) << "modulo 2^32";
  EXPECT_LT(hash_slot(0x9E3779B9U), kHashEntries);
  EXPECT_EQ(kTableBytes, 16384U);
}

TEST(Lz4Block, ShortInputsAreOneLiteralOnlySequence) {
  EXPECT_EQ(compressed({}), bytes_of({0x00})) << "the format's empty block";
  EXPECT_EQ(compressed(bytes_of("A")), bytes_of({0x10, 'A'}));
  EXPECT_EQ(compressed(bytes_of("abcd")), bytes_of({0x40, 'a', 'b', 'c', 'd'}));
  EXPECT_EQ(compressed(bytes_of("abcabcabcabc")),
            concat({bytes_of({0xC0}), bytes_of("abcabcabcabc")}))
      << "twelve bytes repeat, and still cannot be compressed: blocks < 13 bytes cannot";
}

TEST(Lz4Block, ThirteenBytesIsTheShortestBlockWithAMatch) {
  // One literal, then a match of 7 at offset 1 copying bytes it is writing,
  // starting exactly twelve bytes before the end, then the five last literals.
  // The same bytes `lz4 -l` wrote (tests/fixtures/lz4-workload/thirteen-bytes).
  EXPECT_EQ(compressed(bytes_of("aaaaaaaaaaaaa")),
            concat({bytes_of({0x13, 'a', 0x01, 0x00, 0x50}), bytes_of("aaaaa")}));
  EXPECT_EQ(compressed(bytes_of("aaaaaaaaaaaaaa")),
            concat({bytes_of({0x14, 'a', 0x01, 0x00, 0x50}), bytes_of("aaaaa")}));
}

/// Distinct bytes, with the first four repeated at `at`: the only match the
/// input offers.
Bytes one_repeat(std::size_t size, std::size_t at) {
  Bytes input(size);
  for (std::size_t k = 0; k < size; ++k) {
    input[k] = static_cast<std::byte>(k + 1);
  }
  for (std::size_t k = 0; k < 4; ++k) {
    input[at + k] = input[k];
  }
  return input;
}

TEST(Lz4Block, TheLastMatchStartsNoLaterThanTwelveBytesBeforeTheEnd) {
  for (const std::size_t size : {std::size_t{16}, std::size_t{17}, std::size_t{40}}) {
    const Bytes allowed = one_repeat(size, size - 12);
    const testing::Decoded found = testing::decode_block(compressed(allowed), size);
    ASSERT_EQ(found.breach, testing::Breach::kNone) << size;
    EXPECT_EQ(found.bytes, allowed);
    ASSERT_EQ(found.sequences.size(), 2U) << size;
    EXPECT_EQ(found.sequences[0], (Sequence{size - 12, size - 12, 4}));

    const Bytes too_late = one_repeat(size, size - 11);
    EXPECT_EQ(compressed(too_late),
              concat({bytes_of({0xF0, static_cast<unsigned>(size - 15)}), too_late}))
        << size << ": a repeat eleven bytes from the end is left as literals";
  }
}

TEST(Lz4Block, NoMatchReachesTheLastFiveBytes) {
  for (const std::size_t size : {std::size_t{13}, std::size_t{100}, kInputBytes}) {
    const Bytes zeros(size, std::byte{0});
    const testing::Decoded found = testing::decode_block(compressed(zeros), size);
    ASSERT_EQ(found.breach, testing::Breach::kNone);
    EXPECT_EQ(found.bytes, zeros);
    ASSERT_EQ(found.sequences.size(), 2U);
    EXPECT_EQ(found.sequences[0], (Sequence{1, 1, size - 6})) << "one literal, then offset 1";
    EXPECT_EQ(found.sequences[1], (Sequence{5, 0, 0})) << "exactly five last literals";
  }
}

TEST(Lz4Block, EachLengthBoundaryIsCodedByTheCompressorAsTheFormatSays) {
  // A run of `literals` noise bytes, then the same bytes again for `match`
  // bytes (a match at offset `literals`, overlapping itself when it is longer),
  // then a noise tail that does not continue it.
  struct Case {
    std::size_t literals;
    std::size_t match;
  };
  for (const Case c :
       {Case{14, 14}, Case{15, 15}, Case{16, 16}, Case{40, 18}, Case{40, 19}, Case{40, 20},
        Case{269, 273}, Case{270, 274}, Case{271, 275}, Case{525, 529}}) {
    const Bytes run = noise(c.literals, c.literals);
    Bytes input = run;
    for (std::size_t k = 0; k < c.match; ++k) {
      input.push_back(input[k]);
    }
    Bytes tail = noise(16, 99);
    tail[0] = input[c.match] ^ std::byte{0x01};
    input.insert(input.end(), tail.begin(), tail.end());

    const testing::Decoded found = testing::decode_block(compressed(input), input.size());
    ASSERT_EQ(found.breach, testing::Breach::kNone) << c.literals << "/" << c.match;
    EXPECT_EQ(found.bytes, input);
    ASSERT_EQ(found.sequences.size(), 2U) << c.literals << "/" << c.match;
    EXPECT_EQ(found.sequences[0], (Sequence{c.literals, c.literals, c.match}))
        << c.literals << "/" << c.match;
    EXPECT_EQ(found.sequences[1], (Sequence{16, 0, 0}));
  }
}

/// Eight marker bytes, zeros to `at`, the marker again, then a noise tail:
/// the zeros compress to one long match at offset 1, so the table still holds
/// the marker's position 0 when the copy at `at` is reached.
Bytes marker_at(std::size_t at) {
  const Bytes marker = bytes_of("LZ4-mark");
  Bytes input = marker;
  input.resize(at, std::byte{0});
  input.insert(input.end(), marker.begin(), marker.end());
  const Bytes tail = noise(32, 5);
  input.insert(input.end(), tail.begin(), tail.end());
  return input;
}

TEST(Lz4Block, TheFarthestOffsetIsTakenAndTheFirstThatCannotBeCodedIsNot) {
  const Bytes reachable = marker_at(kMaxOffset);
  const testing::Decoded near = testing::decode_block(compressed(reachable), reachable.size());
  ASSERT_EQ(near.breach, testing::Breach::kNone);
  EXPECT_EQ(near.bytes, reachable);
  ASSERT_EQ(near.sequences.size(), 3U);
  EXPECT_EQ(near.sequences[1], (Sequence{0, kMaxOffset, 8})) << "the marker, 65 535 bytes back";

  const Bytes unreachable = marker_at(kMaxOffset + 1);
  const testing::Decoded far = testing::decode_block(compressed(unreachable), unreachable.size());
  ASSERT_EQ(far.breach, testing::Breach::kNone);
  EXPECT_EQ(far.bytes, unreachable);
  ASSERT_EQ(far.sequences.size(), 2U);
  EXPECT_EQ(far.sequences[1].literals, 8U + 32U)
      << "the marker is literals: 65 536 is not an offset";
}

TEST(Lz4Block, WhatTheTableHeldBeforeDoesNotMatter) {
  Bytes input(kInputBytes);
  for (std::size_t k = 0; k < input.size(); ++k) {
    input[k] = static_cast<std::byte>((k * k) >> 7U);
  }
  const Bytes clean = compressed(input, std::byte{0x00});
  EXPECT_EQ(compressed(input, std::byte{0xFF}), clean);
  EXPECT_EQ(compressed(input, std::byte{0x01}), clean) << "small positions that look plausible";
  EXPECT_EQ(compressed(input), clean) << "and again: no state";
}

TEST(Lz4Block, ARepeatIsFoundAtItsDistanceAndExtendedToTheLastLiterals) {
  const Bytes input = noise(1000, 1);
  Bytes repeated = input;
  repeated.insert(repeated.end(), input.begin(), input.end());
  const testing::Decoded found = testing::decode_block(compressed(repeated), repeated.size());
  ASSERT_EQ(found.breach, testing::Breach::kNone);
  EXPECT_EQ(found.bytes, repeated);
  ASSERT_EQ(found.sequences.size(), 2U);
  EXPECT_EQ(found.sequences[0], (Sequence{1000, 1000, 995})) << "five bytes kept back as literals";
}

}  // namespace
}  // namespace loadforge::workloads::compression
