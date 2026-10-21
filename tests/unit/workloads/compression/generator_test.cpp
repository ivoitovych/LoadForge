// SPDX-License-Identifier: GPL-3.0-or-later
//
// The input generator against a second reading of its own description
// (generator.hpp), written here from the header's words: the Philox bytes,
// the walk of literal and copy runs, each draw's three fields. Then the
// properties the workload contract needs of it -- a pure function of (seed,
// index), every index a unit, a prefix for a prefix -- at their boundaries.

#include "workloads/compression/generator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/compression/support.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Bytes;

Bytes generated(std::uint64_t seed, std::uint64_t index, std::size_t size) {
  Bytes input(size, std::byte{0xA5});
  generate_input(seed, index, input);
  return input;
}

/// The header's description, read independently: start from Philox stream
/// `index`; then, with draw j = u64_of(block_at(seed, index, 2^62 + j)),
/// keep 1 + (low byte mod 40) bytes, copy 4 + (next byte mod 61) bytes from
/// 1 + (the bits above mod min(position, 32768)) back, until the end.
Bytes described(std::uint64_t seed, std::uint64_t index, std::size_t size) {
  Bytes out(size);
  rng::fill_stream(seed, index, out);
  std::size_t position = 0;
  for (std::uint64_t j = 0; position < size; ++j) {
    const std::uint64_t bits =
        rng::u64_of(rng::block_at(seed, index, (std::uint64_t{1} << 62U) + j));
    position += 1 + (bits % 256) % 40;
    const std::size_t copy = 4 + ((bits / 256) % 256) % 61;
    const std::size_t back = 1 + (bits / 65536) % (position < 32768 ? position : 32768);
    for (std::size_t k = 0; k < copy && position < size; ++k, ++position) {
      out[position] = out[position - back];
    }
  }
  return out;
}

TEST(CompressionGenerator, IsWhatItsDescriptionSays) {
  for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{1}, ~std::uint64_t{0}}) {
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{9}, ~std::uint64_t{0}}) {
      EXPECT_EQ(generated(seed, index, kInputBytes), described(seed, index, kInputBytes))
          << seed << "/" << index;
    }
  }
}

TEST(CompressionGenerator, TheConstantsAreTheDescribedOnes) {
  EXPECT_EQ(kControlPosition, std::uint64_t{1} << 62U);
  EXPECT_EQ(kMaxLiteralRun, 40U);
  EXPECT_EQ(kMinCopyRun, 4U);
  EXPECT_EQ(kMaxCopyRun, 64U);
  EXPECT_EQ(kMaxDistance, 32768U);
}

TEST(CompressionGenerator, EveryLengthFromNothingThroughOneUnitIsAPrefixOfTheUnit) {
  const Bytes whole = generated(5, 6, kInputBytes);
  for (std::size_t size = 0; size <= 300; ++size) {
    const Bytes part = generated(5, 6, size);
    ASSERT_TRUE(std::ranges::equal(part, std::span<const std::byte>(whole).first(size))) << size;
  }
  EXPECT_EQ(generated(5, 6, kInputBytes - 1), Bytes(whole.begin(), whole.end() - 1));
}

TEST(CompressionGenerator, AnEmptyBufferIsFilledWithNothing) {
  Bytes nothing;
  generate_input(1, 1, nothing);
  EXPECT_TRUE(nothing.empty());
}

TEST(CompressionGenerator, TheFirstRunIsLiteralSoTheFirstByteIsPhilox) {
  for (std::uint64_t index = 0; index < 64; ++index) {
    Bytes philox(1);
    rng::fill_stream(3, index, philox);
    EXPECT_EQ(generated(3, index, 1), philox) << index;
  }
}

TEST(CompressionGenerator, SeedAndIndexEachSelectTheData) {
  const Bytes base = generated(1, 1, kInputBytes);
  EXPECT_EQ(generated(1, 1, kInputBytes), base);
  EXPECT_NE(generated(2, 1, kInputBytes), base);
  EXPECT_NE(generated(1, 2, kInputBytes), base);
  EXPECT_NE(generated(1, 1ULL << 32U, kInputBytes), base) << "the index's high word counts";
  EXPECT_NE(generated(1ULL << 32U, 1, kInputBytes), generated(0, 1, kInputBytes))
      << "the seed's high word counts";
}

TEST(CompressionGenerator, CopiesRepeatEarlierBytesSoTheDataIsNotNoise) {
  const Bytes input = generated(4, 4, kInputBytes);
  Bytes philox(kInputBytes);
  rng::fill_stream(4, 4, philox);
  std::size_t kept = 0;
  for (std::size_t k = 0; k < kInputBytes; ++k) {
    kept += input[k] == philox[k] ? 1U : 0U;
  }
  // Literal runs average 20.5 bytes and copies 34: about 38% of the bytes are
  // the stream's own, and a copied byte equals the stream's one time in 256.
  EXPECT_GT(kept, kInputBytes / 4);
  EXPECT_LT(kept, kInputBytes / 2);
}

}  // namespace
}  // namespace loadforge::workloads::compression
