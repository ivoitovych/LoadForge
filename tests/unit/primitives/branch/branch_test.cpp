// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/branch: the tally against a count made another way,
// the thresholds at both ends of the range, the sorted-equals-unsorted identity,
// the refusal, and the executor contract run against this module's mock.

#include "primitives/branch/branch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/branch/executors.hpp"

namespace loadforge::contracts {
using BranchMockExecutors = ::testing::Types<primitives::branch::mocks::SerialSubject,
                                             primitives::branch::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(BranchMocks, ExecutorContract, BranchMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::branch {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

std::vector<std::uint64_t> values_of(const Config& config, std::uint64_t unit) {
  std::vector<std::uint64_t> values;
  for (std::uint64_t j = 0; j < config.elements; ++j) {
    values.push_back(rng::u64_of(rng::block_at(config.seed, unit, j)));
  }
  return values;
}

/// The oracle: filter, then reduce, with the standard algorithms; no loop that
/// branches per element.
Tally oracle(const Config& config, std::uint64_t unit) {
  const std::vector<std::uint64_t> values = values_of(config, unit);
  std::vector<std::uint64_t> low;
  std::vector<std::uint64_t> high;
  std::partition_copy(values.begin(), values.end(), std::back_inserter(low),
                      std::back_inserter(high),
                      [&config](std::uint64_t v) { return v < config.threshold; });
  return Tally{low.size(), std::accumulate(low.begin(), low.end(), std::uint64_t{0}),
               std::accumulate(high.begin(), high.end(), std::uint64_t{0},
                               [](std::uint64_t a, std::uint64_t b) { return a ^ b; })};
}

constexpr std::uint64_t kMid = std::uint64_t{1} << 63U;

TEST(BranchPrimitive, IsDeclaredBitExact) {
  EXPECT_EQ(kDeterminism, workloads::DeterminismClass::kBitExact);
}

TEST(BranchPrimitive, MatchesTheFilterAndReduceOracle) {
  for (const std::uint64_t elements : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{100}, std::uint64_t{1001}}) {
    for (const std::uint64_t threshold : {std::uint64_t{0}, std::uint64_t{1}, kMid, ~std::uint64_t{0}}) {
      for (const bool sorted : {false, true}) {
        const Config config{.seed = 0xB4A, .elements = elements, .threshold = threshold, .sorted = sorted};
        std::vector<std::uint64_t> scratch(elements);
        EXPECT_EQ(unit_tally(config, 2, scratch), oracle(config, 2))
            << elements << " elements, threshold " << threshold << ", sorted " << sorted;
      }
    }
  }
}

TEST(BranchPrimitive, ThresholdZeroTakesNothingAndTheLargestTakesAllButItself) {
  const Config none{.seed = 1, .elements = 500, .threshold = 0};
  std::vector<std::uint64_t> scratch(500);
  const Tally nothing = unit_tally(none, 0, scratch);
  EXPECT_EQ(nothing.taken, 0U);
  EXPECT_EQ(nothing.below, 0U);
  std::uint64_t all_xor = 0;
  for (const std::uint64_t v : values_of(none, 0)) {
    all_xor ^= v;
  }
  EXPECT_EQ(nothing.above, all_xor);

  const Config all{.seed = 1, .elements = 500, .threshold = ~std::uint64_t{0}};
  const Tally everything = unit_tally(all, 0, scratch);
  EXPECT_EQ(everything.taken, 500U);
  EXPECT_EQ(everything.above, 0U);
}

TEST(BranchPrimitive, AValueEqualToTheThresholdIsNotTaken) {
  // Strictly below: choose the threshold to be a generated value.
  Config config{.seed = 3, .elements = 50};
  const std::vector<std::uint64_t> values = values_of(config, 0);
  config.threshold = values[7];
  std::vector<std::uint64_t> scratch(50);
  const Tally tally = unit_tally(config, 0, scratch);
  EXPECT_EQ(tally.taken, static_cast<std::uint64_t>(std::count_if(values.begin(), values.end(), [&](std::uint64_t v) {
              return v < values[7];
            })));
}

TEST(BranchPrimitive, SortingChangesTheOrderTheBranchSeesAndNothingElse) {
  Config config{.seed = 12, .elements = 777, .threshold = kMid};
  std::vector<std::uint64_t> scratch(777);
  const Tally random_order = unit_tally(config, 4, scratch);
  config.sorted = true;
  EXPECT_EQ(unit_tally(config, 4, scratch), random_order);
}

TEST(BranchPrimitive, CombineIsCommutativeAndAssociative) {
  const Tally a{1, 2, 0xF0};
  const Tally b{3, 5, 0x0F};
  const Tally c{7, ~std::uint64_t{0}, 0xAA};
  EXPECT_EQ(combine(a, b), combine(b, a));
  EXPECT_EQ(combine(combine(a, b), c), combine(a, combine(b, c)));
  EXPECT_EQ(combine(Tally{}, a), a);
  EXPECT_EQ(combine(a, c).below, 1U) << "the sum wraps modulo 2^64";
}

TEST(BranchPrimitive, RefusesScratchSmallerThanEverySlotNeeds) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 1, .elements = 10, .threshold = kMid};
  std::vector<std::uint64_t> scratch(29);
  std::vector<Tally> partials(4, Tally{9, 9, 9});
  const auto result = run(executor, config, scratch, partials);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConfigError::kScratchTooSmall);
  EXPECT_EQ(partials, std::vector<Tally>(4, Tally{9, 9, 9}));
  scratch.resize(30);
  EXPECT_TRUE(run(executor, config, scratch, partials).has_value()) << "exactly enough is enough";
}

TEST(BranchPrimitive, NoElementsNeedsNoScratch) {
  SerialExecutor executor;
  std::vector<Tally> partials(3, Tally{9, 9, 9});
  const auto result = run(executor, Config{.seed = 1, .elements = 0, .threshold = kMid}, {}, partials);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), Tally{});
  EXPECT_EQ(partials, std::vector<Tally>(3, Tally{}));
}

TEST(BranchPrimitive, RunCombinesEachUnitsOracleTally) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 21, .elements = 321, .threshold = kMid, .sorted = false};
  std::vector<std::uint64_t> scratch(3 * 321);
  std::vector<Tally> partials(9);
  const auto total = run(executor, config, scratch, partials);
  ASSERT_TRUE(total.has_value());
  Tally expected;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    EXPECT_EQ(partials[unit], oracle(config, unit)) << unit;
    expected = combine(expected, oracle(config, unit));
  }
  EXPECT_EQ(total.value(), expected);
}

TEST(BranchPrimitive, NoUnitsIsTheEmptyTally) {
  SerialExecutor executor;
  const Config config{.seed = 1, .elements = 4, .threshold = kMid};
  std::vector<std::uint64_t> scratch(4);
  std::vector<Tally> none;
  EXPECT_EQ(run(executor, config, scratch, none).value(), Tally{});
}

}  // namespace
}  // namespace loadforge::primitives::branch
