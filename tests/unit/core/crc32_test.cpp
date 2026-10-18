// SPDX-License-Identifier: GPL-3.0-or-later
//
// CRC-32: pinned to the check value every other implementation agrees on, and
// to the algebra the record framing depends on.

#include "core/crc32.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace loadforge::core {
namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

TEST(Crc32Test, TheCheckValueIsTheOneEveryImplementationAgreesOn) {
  // "123456789" -> 0xCBF43926 is the published check value for CRC-32/ISO-HDLC
  // (zlib, PNG, Ethernet). This is the one assertion that connects the table
  // to the outside world; everything else here is self-consistency.
  EXPECT_EQ(crc32(bytes_of("123456789")), 0xCBF43926U);
}

TEST(Crc32Test, TheEmptyInputIsZeroAndASingleByteIsNot) {
  EXPECT_EQ(crc32({}), 0U);
  EXPECT_EQ(crc32(bytes_of("a")), 0xE8B7BE43U);  // zlib's answer for "a".
}

TEST(Crc32Test, UpdatingOverASplitEqualsTheWholeAtEverySplitPoint) {
  const std::vector<std::byte> whole = bytes_of("The quick brown fox jumps over the lazy dog");
  const std::uint32_t expected = crc32(whole);
  EXPECT_EQ(expected, 0x414FA339U);  // zlib's answer for the sentence.
  const std::span<const std::byte> all{whole};
  for (std::size_t split = 0; split <= whole.size(); ++split) {
    const std::uint32_t first = crc32(all.first(split));
    EXPECT_EQ(crc32_update(first, all.subspan(split)), expected) << "split at " << split;
  }
}

TEST(Crc32Test, EverySingleBitFlipChangesTheValue) {
  // The property the record pipe rests on, at the smallest scale: no one-bit
  // corruption of a frame can keep its checksum.
  const std::vector<std::byte> original = bytes_of("record payload");
  const std::uint32_t expected = crc32(original);
  for (std::size_t index = 0; index < original.size(); ++index) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> flipped = original;
      flipped[index] ^= static_cast<std::byte>(1U << bit);
      EXPECT_NE(crc32(flipped), expected) << "byte " << index << " bit " << bit;
    }
  }
}

TEST(Crc32Test, AByteWithTheHighBitSetIsNotSignExtended) {
  // std::byte to the index: 0xFF must index entry 255, not wrap to a negative.
  const std::vector<std::byte> high{std::byte{0xFF}};
  EXPECT_EQ(crc32(high), 0xFF000000U);  // zlib's answer for the single byte 0xFF.
}

}  // namespace
}  // namespace loadforge::core
