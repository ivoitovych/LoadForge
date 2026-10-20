// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/memory: the digest recomputed from the generator's
// definition (the source is regenerated, never stored), the refusals, the
// slots' separation, and the executor contract run against this module's mock.

#include "primitives/memory/memory.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/memory/executors.hpp"

namespace loadforge::contracts {
using MemoryMockExecutors = ::testing::Types<primitives::memory::mocks::SerialSubject,
                                             primitives::memory::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(MemoryMocks, ExecutorContract, MemoryMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::memory {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

/// Word j of a unit's stream, from the Philox blocks directly: block j / 2,
/// words 0 and 1 for even j, words 2 and 3 for odd j. The definition, not
/// fill_stream and not a byte reassembly.
std::uint64_t word_of(const Config& config, std::uint64_t unit, std::uint64_t j) {
  const rng::Block b = rng::block_at(config.seed, unit, j / 2);
  return j % 2 == 0 ? (std::uint64_t{b[0]} | std::uint64_t{b[1]} << 32U)
                    : (std::uint64_t{b[2]} | std::uint64_t{b[3]} << 32U);
}

std::uint64_t oracle(const Config& config, std::uint64_t unit) {
  std::uint64_t sum = 0;
  for (std::uint64_t j = 0; j < config.bytes / 8; ++j) {
    sum += word_of(config, unit, j);
  }
  std::uint64_t digest = 0;
  for (std::uint64_t pass = 0; pass < config.passes; ++pass) {
    digest += std::rotl(sum, static_cast<int>(pass % 64));
  }
  return digest;
}

TEST(MemoryPrimitive, IsDeclaredBitExact) {
  EXPECT_EQ(kDeterminism, workloads::DeterminismClass::kBitExact);
}

TEST(MemoryPrimitive, ScratchIsASourceAndADestination) {
  EXPECT_EQ(scratch_words(Config{.bytes = 1024}).value(), 256U);
  EXPECT_EQ(scratch_words(Config{.bytes = 0}).value(), 0U);
}

TEST(MemoryPrimitive, PartialWordsAreRefused) {
  for (const std::uint64_t bytes : {1ULL, 7ULL, 9ULL, 1023ULL}) {
    EXPECT_EQ(scratch_words(Config{.bytes = bytes}).error(), ConfigError::kBytesNotWholeWords)
        << bytes;
  }
}

TEST(MemoryPrimitive, MatchesTheDefinitionAtEdgeSizes) {
  // Empty, one word (half a Philox block), one block, a block and a half, and
  // a buffer many blocks long; with no pass, one, and many (the rotation wraps
  // past 64).
  for (const std::uint64_t bytes : {0ULL, 8ULL, 16ULL, 24ULL, 32ULL, 4096ULL}) {
    for (const std::uint64_t passes : {0ULL, 1ULL, 2ULL, 64ULL, 70ULL}) {
      const Config config{.seed = 0xBEEF, .bytes = bytes, .passes = passes};
      std::vector<std::uint64_t> scratch(scratch_words(config).value());
      EXPECT_EQ(unit_digest(config, 6, scratch), oracle(config, 6))
          << bytes << " bytes, " << passes << " passes";
    }
  }
}

TEST(MemoryPrimitive, TheSourceIsTheStreamAndTheCopyEqualsIt) {
  const Config config{.seed = 2, .bytes = 160, .passes = 3};
  std::vector<std::uint64_t> scratch(scratch_words(config).value(), 0);
  (void)unit_digest(config, 1, scratch);
  for (std::uint64_t j = 0; j < 20; ++j) {
    EXPECT_EQ(scratch[j], word_of(config, 1, j)) << "source word " << j;
    EXPECT_EQ(scratch[20 + j], scratch[j]) << "copy word " << j;
  }
}

TEST(MemoryPrimitive, OnlyTheScratchItWasGivenIsWritten) {
  const Config config{.seed = 2, .bytes = 64, .passes = 1};
  std::vector<std::uint64_t> scratch(scratch_words(config).value() + 6, 0x5A5A5A5A5A5A5A5AULL);
  (void)unit_digest(config, 0, std::span<std::uint64_t>(scratch).first(16));
  for (std::size_t i = 16; i < scratch.size(); ++i) {
    EXPECT_EQ(scratch[i], 0x5A5A5A5A5A5A5A5AULL) << i;
  }
}

TEST(MemoryPrimitive, RefusesBadConfigurationAndSmallScratchWithoutRunning) {
  ThreadedExecutor executor(3);
  std::vector<std::uint64_t> partials(4, 0xAAAA);
  std::vector<std::uint64_t> scratch(1000);
  const auto bad = run(executor, Config{.bytes = 12}, scratch, partials);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), ConfigError::kBytesNotWholeWords);

  const Config config{.seed = 1, .bytes = 64, .passes = 2};  // 16 words a slot
  std::vector<std::uint64_t> small(3 * 16 - 1);
  const auto few = run(executor, config, small, partials);
  ASSERT_FALSE(few.has_value());
  EXPECT_EQ(few.error(), ConfigError::kScratchTooSmall);
  EXPECT_EQ(partials, std::vector<std::uint64_t>(4, 0xAAAA));
  small.resize(3 * 16);
  EXPECT_TRUE(run(executor, config, small, partials).has_value()) << "exactly enough is enough";
}

TEST(MemoryPrimitive, ZeroBytesRunsWithNoScratchAtAll) {
  SerialExecutor executor;
  std::vector<std::uint64_t> partials(5, 0xAAAA);
  const auto total = run(executor, Config{.seed = 1, .bytes = 0, .passes = 9}, {}, partials);
  ASSERT_TRUE(total.has_value());
  EXPECT_EQ(total.value(), 0U);
  EXPECT_EQ(partials, std::vector<std::uint64_t>(5, 0));
}

TEST(MemoryPrimitive, RunSumsEachUnitsOracleDigestInOrder) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 44, .bytes = 512, .passes = 4};
  std::vector<std::uint64_t> scratch(3 * scratch_words(config).value());
  std::vector<std::uint64_t> partials(8, 0);
  const auto total = run(executor, config, scratch, partials);
  ASSERT_TRUE(total.has_value());
  std::uint64_t expected = 0;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    EXPECT_EQ(partials[unit], oracle(config, unit)) << unit;
    expected += partials[unit];
  }
  EXPECT_EQ(total.value(), expected);
}

TEST(MemoryPrimitive, NoUnitsIsZero) {
  SerialExecutor executor;
  const Config config{.seed = 1, .bytes = 64, .passes = 1};
  std::vector<std::uint64_t> scratch(scratch_words(config).value());
  std::vector<std::uint64_t> none;
  EXPECT_EQ(run(executor, config, scratch, none).value(), 0U);
}

}  // namespace
}  // namespace loadforge::primitives::memory
