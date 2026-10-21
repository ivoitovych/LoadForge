// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/cache, declared BitExact (docs/determinism.md §5):
// byte-identical at 1, 2, 3, 4 and 7 threads, across repeats, and across the
// line size, which changes where nodes sit but never what the chase finds.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/cache/cache.hpp"
#include "unit/primitives/cache/executors.hpp"

namespace loadforge::primitives::cache {
namespace {

using mocks::ThreadedExecutor;

struct Outcome {
  std::uint64_t total;
  std::vector<std::uint64_t> partials;
  [[nodiscard]] bool operator==(const Outcome&) const = default;
};

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units) {
  ThreadedExecutor executor(threads);
  std::vector<std::uint32_t> scratch(threads * scratch_words(config).value());
  Outcome outcome{0, std::vector<std::uint64_t>(units)};
  outcome.total = run(executor, config, scratch, outcome.partials).value();
  return outcome;
}

TEST(CacheDeterminism, ByteIdenticalAcrossThreadCounts) {
  const Config config{.seed = 0xC4C4E, .nodes = 311, .line_bytes = 64, .steps = 4001};
  const Outcome one = outcome_at(1, config, 17);
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    EXPECT_EQ(outcome_at(threads, config, 17), one) << threads << " threads";
  }
}

TEST(CacheDeterminism, IdenticalAcrossRepeatRuns) {
  const Config config{.seed = 3, .nodes = 100, .line_bytes = 128, .steps = 999};
  const Outcome first = outcome_at(7, config, 11);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 11), first);
  }
}

TEST(CacheDeterminism, IdenticalAcrossLineSizes) {
  Config config{.seed = 8, .nodes = 150, .line_bytes = 32, .steps = 2222};
  const Outcome at_32 = outcome_at(3, config, 9);
  for (const std::uint32_t line : {4U, 64U, 128U, 256U}) {
    config.line_bytes = line;
    EXPECT_EQ(outcome_at(3, config, 9), at_32) << "line " << line;
  }
}

}  // namespace
}  // namespace loadforge::primitives::cache
