// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/branch, declared BitExact (docs/determinism.md §5):
// byte-identical at 1, 2, 3, 4 and 7 threads, across repeats, and across the
// ordering the branch is shown.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/branch/branch.hpp"
#include "unit/primitives/branch/executors.hpp"

namespace loadforge::primitives::branch {
namespace {

using mocks::ThreadedExecutor;

struct Outcome {
  Tally total;
  std::vector<Tally> partials;
  [[nodiscard]] bool operator==(const Outcome&) const = default;
};

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units) {
  ThreadedExecutor executor(threads);
  std::vector<std::uint64_t> scratch(threads * config.elements);
  Outcome outcome{Tally{}, std::vector<Tally>(units)};
  outcome.total = run(executor, config, scratch, outcome.partials).value();
  return outcome;
}

constexpr std::uint64_t kMid = std::uint64_t{1} << 63U;

TEST(BranchDeterminism, ByteIdenticalAcrossThreadCounts) {
  const Config config{.seed = 0xB4A, .elements = 999, .threshold = kMid};
  const Outcome one = outcome_at(1, config, 19);
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    EXPECT_EQ(outcome_at(threads, config, 19), one) << threads << " threads";
  }
}

TEST(BranchDeterminism, IdenticalAcrossRepeatRuns) {
  const Config config{.seed = 5, .elements = 400, .threshold = kMid / 3};
  const Outcome first = outcome_at(7, config, 13);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 13), first);
  }
}

TEST(BranchDeterminism, IdenticalWhetherOrNotTheValuesAreSorted) {
  Config config{.seed = 8, .elements = 600, .threshold = kMid};
  const Outcome unsorted = outcome_at(3, config, 9);
  config.sorted = true;
  EXPECT_EQ(outcome_at(3, config, 9), unsorted);
}

}  // namespace
}  // namespace loadforge::primitives::branch
