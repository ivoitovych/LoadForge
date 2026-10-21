// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/integer, declared BitExact (docs/determinism.md §5):
// byte-identical output at 1, 2, 3, 4 and 7 threads, and across repeat runs.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/integer/integer.hpp"
#include "unit/primitives/integer/executors.hpp"

namespace loadforge::primitives::integer {
namespace {

using mocks::ThreadedExecutor;

struct Outcome {
  std::uint64_t total;
  std::vector<std::uint64_t> partials;
  [[nodiscard]] bool operator==(const Outcome&) const = default;
};

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units) {
  ThreadedExecutor executor(threads);
  Outcome outcome{0, std::vector<std::uint64_t>(units)};
  outcome.total = run(executor, config, outcome.partials);
  return outcome;
}

TEST(IntegerDeterminism, ByteIdenticalAcrossThreadCounts) {
  const Config config{.seed = 0xABCDEF, .steps = 1500};
  const Outcome one = outcome_at(1, config, 23);
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    EXPECT_EQ(outcome_at(threads, config, 23), one) << threads << " threads";
  }
}

TEST(IntegerDeterminism, IdenticalAcrossRepeatRuns) {
  const Config config{.seed = 42, .steps = 800};
  const Outcome first = outcome_at(7, config, 31);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 31), first);
  }
}

TEST(IntegerDeterminism, MoreThreadsThanUnitsStillIdentical) {
  const Config config{.seed = 1, .steps = 10};
  EXPECT_EQ(outcome_at(7, config, 2), outcome_at(1, config, 2));
}

}  // namespace
}  // namespace loadforge::primitives::integer
