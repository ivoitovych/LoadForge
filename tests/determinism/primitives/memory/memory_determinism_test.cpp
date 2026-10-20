// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/memory, declared BitExact (docs/determinism.md §5):
// byte-identical at 1, 2, 3, 4 and 7 threads and across repeats; and the
// scratch's prior content never reaches the digest.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/memory/memory.hpp"
#include "unit/primitives/memory/executors.hpp"

namespace loadforge::primitives::memory {
namespace {

using mocks::ThreadedExecutor;

struct Outcome {
  std::uint64_t total;
  std::vector<std::uint64_t> partials;
  [[nodiscard]] bool operator==(const Outcome&) const = default;
};

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units,
                   std::uint64_t scratch_fill = 0) {
  ThreadedExecutor executor(threads);
  std::vector<std::uint64_t> scratch(threads * scratch_words(config).value(), scratch_fill);
  Outcome outcome{0, std::vector<std::uint64_t>(units)};
  outcome.total = run(executor, config, scratch, outcome.partials).value();
  return outcome;
}

TEST(MemoryDeterminism, ByteIdenticalAcrossThreadCounts) {
  const Config config{.seed = 0x3E3, .bytes = 2048, .passes = 5};
  const Outcome one = outcome_at(1, config, 21);
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    EXPECT_EQ(outcome_at(threads, config, 21), one) << threads << " threads";
  }
}

TEST(MemoryDeterminism, IdenticalAcrossRepeatRuns) {
  const Config config{.seed = 6, .bytes = 800, .passes = 3};
  const Outcome first = outcome_at(7, config, 14);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 14), first);
  }
}

TEST(MemoryDeterminism, StaleScratchDoesNotChangeTheResult) {
  const Config config{.seed = 6, .bytes = 800, .passes = 3};
  EXPECT_EQ(outcome_at(3, config, 9, ~std::uint64_t{0}), outcome_at(3, config, 9, 0));
}

}  // namespace
}  // namespace loadforge::primitives::memory
