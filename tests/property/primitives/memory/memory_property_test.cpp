// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/memory: properties over generated cases.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "primitives/memory/memory.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/memory/executors.hpp"

namespace loadforge::primitives::memory {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 120;

Config case_config(std::uint64_t index) {
  const rng::Block draw = rng::block_at(0x3E30, 1, index);
  return Config{.seed = rng::u64_of(draw), .bytes = 8 * (draw[2] % 130), .passes = draw[3] % 9};
}

TEST(MemoryProperties, ByteMultiplesOfEightAreExactlyTheAcceptedSizes) {
  for (std::uint64_t bytes = 0; bytes < 80; ++bytes) {
    EXPECT_EQ(scratch_words(Config{.bytes = bytes}).has_value(), bytes % 8 == 0) << bytes;
  }
}

TEST(MemoryProperties, PrefixOfALongerBufferIsTheShorterBuffer) {
  // fill_stream's prefix property carried through: the shorter unit's source
  // is the longer unit's source's start, so with one pass the digest of the
  // longer buffer minus the shorter's is the sum of the extra words.
  for (std::uint64_t index = 0; index < kCases; ++index) {
    Config shorter = case_config(index);
    shorter.passes = 1;
    Config longer = shorter;
    longer.bytes += 16;
    std::vector<std::uint64_t> a(scratch_words(shorter).value());
    std::vector<std::uint64_t> b(scratch_words(longer).value());
    const std::uint64_t digest_short = unit_digest(shorter, 3, a);
    const std::uint64_t digest_long = unit_digest(longer, 3, b);
    const std::size_t words = shorter.bytes / 8;
    EXPECT_EQ(digest_long - digest_short, b[words] + b[words + 1]) << "case " << index;
  }
}

TEST(MemoryProperties, EveryExecutorAndSlotCountGivesTheSameDigests) {
  for (std::uint64_t index = 0; index < kCases / 4; ++index) {
    const Config config = case_config(index);
    const std::size_t units = 1 + index % 7;
    const std::size_t threads = 1 + index % 5;
    SerialExecutor serial;
    ThreadedExecutor threaded(threads);
    std::vector<std::uint64_t> serial_scratch(scratch_words(config).value());
    std::vector<std::uint64_t> threaded_scratch(threads * scratch_words(config).value());
    std::vector<std::uint64_t> a(units);
    std::vector<std::uint64_t> b(units);
    EXPECT_EQ(run(serial, config, serial_scratch, a).value(),
              run(threaded, config, threaded_scratch, b).value());
    EXPECT_EQ(a, b) << "case " << index;
  }
}

}  // namespace
}  // namespace loadforge::primitives::memory
