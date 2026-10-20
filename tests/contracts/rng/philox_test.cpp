// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generator contract's own tests. The contract carries its definition
// (src/primitives/rng/philox.hpp says why), so these test the definition: the
// published known-answer vectors, read from the fixture copied verbatim from
// Random123, and the addressing and serialisation conventions built on it.

#include "primitives/rng/philox.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace loadforge::rng {
namespace {

struct Vector {
  Block counter;
  Key key;
  Block expected;
};

std::vector<Vector> published_vectors() {
  std::ifstream in(std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/rng/philox4x32-10.kat");
  std::vector<Vector> vectors;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream fields(line);
    std::string name;
    int rounds = 0;
    fields >> name >> std::dec >> rounds >> std::hex;
    Vector vector{};
    for (auto& word : vector.counter) fields >> word;
    for (auto& word : vector.key) fields >> word;
    for (auto& word : vector.expected) fields >> word;
    EXPECT_EQ(name, "philox4x32");
    EXPECT_EQ(rounds, kPhiloxRounds);
    EXPECT_FALSE(fields.fail()) << line;
    vectors.push_back(vector);
  }
  return vectors;
}

TEST(PhiloxDefinition, MatchesEveryPublishedKnownAnswer) {
  const std::vector<Vector> vectors = published_vectors();
  ASSERT_EQ(vectors.size(), 3U) << "the fixture holds Random123's three philox4x32-10 vectors";
  for (const Vector& vector : vectors) {
    EXPECT_EQ(philox4x32_10(vector.counter, vector.key), vector.expected);
  }
}

TEST(PhiloxDefinition, IsAConstantExpression) {
  // The all-zero vector, evaluated by the compiler: the definition is usable
  // in constexpr context, and agrees with itself there.
  constexpr Block kZero = philox4x32_10(Block{0, 0, 0, 0}, Key{0, 0});
  static_assert(kZero == Block{0x6627e8d5U, 0xe169c58dU, 0xbc57ac4cU, 0x9b00dbd8U});
  EXPECT_EQ(kZero[0], 0x6627e8d5U);
}

TEST(PhiloxAddressing, TheSeedIsTheKeyLowWordFirst) {
  EXPECT_EQ(key_of(0x0123456789ABCDEFULL), (Key{0x89ABCDEFU, 0x01234567U}));
  EXPECT_EQ(key_of(0), (Key{0, 0}));
}

TEST(PhiloxAddressing, PositionIsTheLowCounterWordsAndStreamTheHigh) {
  const std::uint64_t seed = 0x299f31d0a4093822ULL;
  // The third published vector, reached through the addressing convention:
  // counter {243f6a88, 85a308d3, 13198a2e, 03707344} is position
  // 0x85a308d3243f6a88 of stream 0x0370734413198a2e.
  EXPECT_EQ(block_at(seed, 0x0370734413198a2eULL, 0x85a308d3243f6a88ULL),
            (Block{0xd16cfe09U, 0x94fdccebU, 0x5001e420U, 0x24126ea1U}));
  EXPECT_NE(block_at(seed, 1, 0), block_at(seed, 0, 1)) << "streams and positions do not alias";
  EXPECT_NE(block_at(seed, 0, 0), block_at(seed + 1, 0, 0)) << "seeds select different data";
}

TEST(PhiloxFillStream, SerialisesEachWordLittleEndianWhateverTheMachine) {
  std::array<std::byte, 16> bytes{};
  fill_stream(0, 0, bytes);
  // Block {6627e8d5, e169c58d, ...}: word 0 low byte first.
  EXPECT_EQ(bytes[0], std::byte{0xd5});
  EXPECT_EQ(bytes[1], std::byte{0xe8});
  EXPECT_EQ(bytes[2], std::byte{0x27});
  EXPECT_EQ(bytes[3], std::byte{0x66});
  EXPECT_EQ(bytes[4], std::byte{0x8d});
  EXPECT_EQ(bytes[15], std::byte{0x9b});
}

TEST(PhiloxFillStream, APrefixOfTheStreamIsThePrefixOfALongerFill) {
  std::vector<std::byte> whole(49);
  fill_stream(7, 3, whole);
  for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16},
                                   std::size_t{17}, std::size_t{33}, std::size_t{48}}) {
    std::vector<std::byte> prefix(length, std::byte{0xEE});
    fill_stream(7, 3, prefix);
    EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), whole.begin())) << "length " << length;
  }
  // Byte 16 starts the second block.
  const Block second = block_at(7, 3, 1);
  EXPECT_EQ(whole[16], static_cast<std::byte>(second[0] & 0xFFU));
  EXPECT_EQ(whole[48], static_cast<std::byte>(block_at(7, 3, 3)[0] & 0xFFU));
}

TEST(PhiloxFillStream, AnEmptySpanIsFilledWithNothing) {
  std::span<std::byte> nothing;
  fill_stream(1, 2, nothing);
  EXPECT_TRUE(nothing.empty());
}

TEST(PhiloxValues, U64IsWordZeroLow) {
  EXPECT_EQ(u64_of(Block{0x89ABCDEFU, 0x01234567U, 0xFFFFFFFFU, 0xFFFFFFFFU}),
            0x0123456789ABCDEFULL);
}

TEST(PhiloxValues, TheUnitIntervalIsHalfOpenAndExact) {
  EXPECT_EQ(unit_interval(0), 0.0);
  EXPECT_EQ(unit_interval(~std::uint64_t{0}), 1.0 - 1.0 / 9007199254740992.0)
      << "the largest value is 1 - 2^-53, never 1";
  EXPECT_EQ(unit_interval(std::uint64_t{1} << 63U), 0.5);
  EXPECT_EQ(unit_interval(0x7FFULL), 0.0) << "the low 11 bits are discarded";
  EXPECT_EQ(unit_interval(0x800ULL), 1.0 / 9007199254740992.0);
}

}  // namespace
}  // namespace loadforge::rng
