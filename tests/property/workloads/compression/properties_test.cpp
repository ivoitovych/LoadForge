// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for the compression workload, over many inputs rather than chosen ones:
//
//   * the block never exceeds compress_bound(n), including on the inputs that
//     come closest to it -- no matches at all, and matches so short and so
//     far apart that each sequence costs as much as it covers or more;
//   * the generator's data compresses to between a fifth and four fifths of
//     its size (spec.hpp), over units of several seeds;
//   * every input round-trips through the test decoder: every length either
//     side of each boundary the format or the compressor has, in each of
//     several shapes of data.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
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

using testing::Bytes;
using testing::noise;

/// A named way of making n bytes.
struct Shape {
  std::string name;
  std::function<Bytes(std::size_t)> make;
};

/// `literals` fresh bytes, then a copy of the first four of them, repeated:
/// each sequence is that many literals and a four-byte match, the costliest
/// sequence a match can be.
Bytes short_matches(std::size_t size, std::size_t literals) {
  Bytes out;
  out.reserve(size + literals + 4);
  std::uint64_t round = 0;
  while (out.size() < size) {
    const Bytes fresh = noise(literals, 1000 + round++);
    out.insert(out.end(), fresh.begin(), fresh.end());
    out.insert(out.end(), fresh.begin(), fresh.begin() + 4);
  }
  out.resize(size);
  return out;
}

std::vector<Shape> shapes() {
  return {
      {"zeros", [](std::size_t n) { return Bytes(n, std::byte{0}); }},
      {"noise", [](std::size_t n) { return noise(n, 17); }},
      {"period 3",
       [](std::size_t n) {
         Bytes out(n);
         for (std::size_t k = 0; k < n; ++k) {
           out[k] = static_cast<std::byte>("abc"[k % 3]);
         }
         return out;
       }},
      {"ramp",
       [](std::size_t n) {
         Bytes out(n);
         for (std::size_t k = 0; k < n; ++k) {
           out[k] = static_cast<std::byte>(k % 251);
         }
         return out;
       }},
      {"15 literals and a 4-byte match", [](std::size_t n) { return short_matches(n, 15); }},
      {"270 literals and a 4-byte match", [](std::size_t n) { return short_matches(n, 270); }},
      {"generated",
       [](std::size_t n) {
         Bytes out(n);
         generate_input(31, 4, out);
         return out;
       }},
  };
}

void expect_round_trip(const std::string& name, std::span<const std::byte> input) {
  const Bytes block = testing::compressed(input);
  EXPECT_LE(block.size(), compress_bound(input.size())) << name << " of " << input.size();
  const testing::Decoded decoded = testing::decode_block(block, input.size());
  ASSERT_EQ(decoded.breach, testing::Breach::kNone)
      << name << " of " << input.size() << ": " << describe(decoded.breach) << " at " << decoded.at;
  ASSERT_TRUE(std::ranges::equal(decoded.bytes, input)) << name << " of " << input.size();
}

TEST(CompressionProperty, EveryLengthUpToElevenHundredRoundTripsWithinTheBound) {
  for (const Shape& shape : shapes()) {
    const Bytes whole = shape.make(1100);
    for (std::size_t size = 0; size <= whole.size(); ++size) {
      expect_round_trip(shape.name, std::span<const std::byte>(whole).first(size));
    }
  }
}

TEST(CompressionProperty, EachSideOfEveryLargerBoundaryRoundTripsWithinTheBound) {
  // The table's size, a full unit, and the first offset that cannot be coded.
  for (const Shape& shape : shapes()) {
    for (const std::size_t size :
         {std::size_t{4095}, std::size_t{4096}, std::size_t{4097}, kInputBytes - 1, kInputBytes,
          kInputBytes + 1, 2 * kInputBytes}) {
      expect_round_trip(shape.name, shape.make(size));
    }
  }
}

TEST(CompressionProperty, TheBoundIsApproachedWhereTheFormatSaysAndNeverPassed) {
  // No match at all: a token, the literal length's bytes, the literals. The
  // length's bytes are at most 1 + n / 255, so with the token this block uses
  // at most two of the bound's 16 bytes of slack -- both at n = 15, where the
  // length needs its zero byte and n / 255 is still 0, and wherever the
  // length's bytes and n / 255 round to the same count after it.
  for (const std::size_t size : {std::size_t{14}, std::size_t{15}, std::size_t{269},
                                 std::size_t{270}, std::size_t{1000}, kInputBytes}) {
    const Bytes block = testing::compressed(noise(size, 23));
    const std::size_t length_bytes = size < 15 ? 0 : 1 + (size - 15) / 255;
    EXPECT_EQ(block.size(), 1 + length_bytes + size) << size;
    EXPECT_GE(compress_bound(size) - block.size(), 14U) << size;
  }
  // Short matches far apart: a 270-literal sequence costs one byte more than
  // it covers, and still stays inside n / 255.
  const Bytes costly = short_matches(kInputBytes, 270);
  const Bytes block = testing::compressed(costly);
  EXPECT_GT(block.size(), kInputBytes) << "the input really is one the format expands";
  EXPECT_LE(block.size(), compress_bound(kInputBytes));
}

TEST(CompressionProperty, GeneratedUnitsCompressToBetweenAFifthAndFourFifths) {
  std::size_t smallest = kInputBytes;
  std::size_t largest = 0;
  Bytes input(kInputBytes);
  for (const std::uint64_t seed :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xDEADBEEF}, ~std::uint64_t{0}}) {
    for (std::uint64_t index = 0; index < 32; ++index) {
      generate_input(seed, index * 0x9E3779B97F4A7C15ULL, input);
      const std::size_t length = testing::compressed(input).size();
      smallest = std::min(smallest, length);
      largest = std::max(largest, length);
      EXPECT_GE(length * 5, kInputBytes) << seed << "/" << index;
      EXPECT_LE(length * 5, 4 * kInputBytes) << seed << "/" << index;
    }
  }
  // Where the generator lands, so a change that moved it towards an edge is
  // seen before it crosses one: 0.57 to 0.61 of the input when written.
  EXPECT_GT(smallest * 2, kInputBytes) << smallest;
  EXPECT_LT(largest * 3, 2 * kInputBytes) << largest;
}

TEST(CompressionProperty, EveryUnitHasBothLiteralsAndMatches) {
  Bytes input(kInputBytes);
  for (std::uint64_t index = 0; index < 8; ++index) {
    generate_input(77, index, input);
    const testing::Decoded decoded = testing::decode_block(testing::compressed(input), kInputBytes);
    ASSERT_EQ(decoded.breach, testing::Breach::kNone);
    std::size_t literals = 0;
    std::size_t matched = 0;
    for (const testing::Sequence& sequence : decoded.sequences) {
      literals += sequence.literals;
      matched += sequence.match;
    }
    EXPECT_EQ(literals + matched, kInputBytes);
    EXPECT_GT(literals, kInputBytes / 5) << index;
    EXPECT_GT(matched, kInputBytes / 5) << index;
  }
}

}  // namespace
}  // namespace loadforge::workloads::compression
