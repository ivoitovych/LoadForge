// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the sort specification carries. Expected values are written
// out, never computed by the function under test; the Philox words are
// Random123's known-answer vectors (tests/fixtures/rng/philox4x32-10.kat).

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "primitives/rng/philox.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

TEST(SortSpecDefinition, NameAndVersion) {
  EXPECT_EQ(sort::kName, "sort");
  EXPECT_TRUE(is_valid_name(sort::kName));
  EXPECT_EQ(sort::kVersion, 1U);
}

TEST(SortSpecDefinition, SizesAndLayout) {
  EXPECT_EQ(sort::kKeys, 131072U);
  EXPECT_EQ(sort::kInputBytes, 1048576U);
  EXPECT_EQ(sort::kIndexRegionOffset, 1048576U);
  EXPECT_EQ(sort::kOutputBytes, 1572864U);
  EXPECT_EQ(sort::key_offset(0), 0U);
  EXPECT_EQ(sort::key_offset(1), 8U);
  EXPECT_EQ(sort::key_offset(131071), 1048568U);
  EXPECT_EQ(sort::index_offset(0), 1048576U);
  EXPECT_EQ(sort::index_offset(1), 1048580U);
  EXPECT_EQ(sort::index_offset(131071), 1572864U - 4U) << "the last index ends the output";
  EXPECT_LE(sort::kKeys - 1, std::uint64_t{std::numeric_limits<std::uint32_t>::max()})
      << "every input position fits an index";
}

TEST(SortSpecDefinition, KeysAreTheFirstTwoWordsOfOneBlockPerPosition) {
  // Counter 0, key 0: 6627e8d5 e169c58d bc57ac4c 9b00dbd8.
  EXPECT_EQ(sort::key_at(0, 0, 0), 0xE169C58D6627E8D5ULL);
  // Every counter and key word ffffffff: 408f276d 41c83b0e a20bc7c6 6d5451fd.
  constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};
  EXPECT_EQ(sort::key_at(kAllOnes, kAllOnes, kAllOnes), 0x41C83B0E408F276DULL);
  // The arguments are (seed, unit, position) in block_at's order.
  EXPECT_EQ(sort::key_at(7, 3, 11), rng::u64_of(rng::block_at(7, 3, 11)));
  EXPECT_NE(sort::key_at(7, 3, 11), sort::key_at(7, 11, 3));
  EXPECT_NE(sort::key_at(7, 3, 11), sort::key_at(3, 7, 11));
}

TEST(SortSpecDefinition, ChunkCountsAreThePowersOfTwoUpTo64) {
  for (const std::uint32_t chunks : {1U, 2U, 4U, 8U, 16U, 32U, 64U}) {
    EXPECT_TRUE(sort::is_chunk_count(chunks)) << chunks;
    EXPECT_EQ(sort::kKeys % chunks, 0U) << "every range holds kKeys / P positions";
  }
  for (const std::uint32_t chunks : {0U, 3U, 48U, 128U}) {
    EXPECT_FALSE(sort::is_chunk_count(chunks)) << chunks;
  }
  EXPECT_EQ(sort::kDefaultChunkCount, 16U);
  EXPECT_TRUE(sort::is_chunk_count(sort::kDefaultChunkCount));
}

TEST(SortSpecDefinition, LoadsAreLittleEndianAtAnyOffset) {
  const std::array<std::byte, 10> bytes = {
      std::byte{0xEE}, std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
      std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}, std::byte{0xEE}};
  EXPECT_EQ(sort::load_le64(bytes, 1), 0xEFCDAB8967452301ULL);
  EXPECT_EQ(sort::load_le64(bytes, 2), 0xEEEFCDAB89674523ULL);
  EXPECT_EQ(sort::load_le32(bytes, 1), 0x67452301U);
  EXPECT_EQ(sort::load_le32(bytes, 6), 0xEEEFCDABU);
}

TEST(SortSpecDefinition, EntriesOrderByUnsignedKeyThenByIndex) {
  using sort::Entry;
  using sort::precedes;
  EXPECT_TRUE(precedes(Entry{1, 9}, Entry{2, 0})) << "the key decides first";
  EXPECT_FALSE(precedes(Entry{2, 0}, Entry{1, 9}));
  EXPECT_TRUE(precedes(Entry{5, 3}, Entry{5, 4})) << "equal keys: the input position";
  EXPECT_FALSE(precedes(Entry{5, 4}, Entry{5, 3}));
  EXPECT_FALSE(precedes(Entry{5, 4}, Entry{5, 4})) << "strict";
  EXPECT_TRUE(precedes(Entry{0x7FFFFFFFFFFFFFFFULL, 0}, Entry{0x8000000000000000ULL, 0}))
      << "unsigned: the top bit is the largest, not a sign";
  EXPECT_TRUE(precedes(Entry{0, 0}, Entry{~std::uint64_t{0}, 0}));
}

}  // namespace
}  // namespace loadforge::workloads
