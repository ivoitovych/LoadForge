// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/cache: properties over generated cases.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "primitives/cache/cache.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/cache/executors.hpp"

namespace loadforge::primitives::cache {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 150;

Config case_config(std::uint64_t index) {
  const rng::Block draw = rng::block_at(0xCAC4E, 1, index);
  static constexpr std::uint32_t kLines[] = {4, 8, 16, 32, 64, 128, 256};
  return Config{.seed = rng::u64_of(draw),
                .nodes = 2 + draw[2] % 200,
                .line_bytes = kLines[draw[3] % 7],
                .steps = draw[3] / 7 % 700};
}

TEST(CacheProperties, TheDigestIsAlwaysANodeOfTheCycle) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const Config config = case_config(index);
    std::vector<std::uint32_t> scratch(scratch_words(config).value());
    EXPECT_LT(unit_digest(config, index, scratch), config.nodes) << "case " << index;
  }
}

TEST(CacheProperties, StepsAreAdditiveAlongTheCycle) {
  // The chase is a walk on a permutation: steps a then b ends where a + b
  // steps end, so a lap of `nodes` steps is the identity.
  for (std::uint64_t index = 0; index < kCases; ++index) {
    Config config = case_config(index);
    std::vector<std::uint32_t> scratch(scratch_words(config).value());
    const std::uint64_t base = unit_digest(config, 0, scratch);
    config.steps += config.nodes;
    EXPECT_EQ(unit_digest(config, 0, scratch), base) << "case " << index;
  }
}

TEST(CacheProperties, DigestsAgreeAcrossExecutorsAndSlotCounts) {
  for (std::uint64_t index = 0; index < kCases / 5; ++index) {
    const Config config = case_config(index);
    const std::size_t units = 1 + index % 8;
    const std::size_t threads = 1 + index % 5;
    SerialExecutor serial;
    ThreadedExecutor threaded(threads);
    std::vector<std::uint32_t> serial_scratch(scratch_words(config).value());
    std::vector<std::uint32_t> threaded_scratch(threads * scratch_words(config).value());
    std::vector<std::uint64_t> a(units);
    std::vector<std::uint64_t> b(units);
    EXPECT_EQ(run(serial, config, serial_scratch, a).value(),
              run(threaded, config, threaded_scratch, b).value());
    EXPECT_EQ(a, b) << "case " << index;
  }
}

TEST(CacheProperties, AnyBadLineSizeIsRefusedAndAnyGoodOneAccepted) {
  for (std::uint32_t line = 0; line <= kMaxLineBytes + 8; ++line) {
    const bool good = line != 0 && line % 4 == 0 && line <= kMaxLineBytes;
    EXPECT_EQ(scratch_words(Config{.nodes = 2, .line_bytes = line}).has_value(), good) << line;
  }
}

}  // namespace
}  // namespace loadforge::primitives::cache
