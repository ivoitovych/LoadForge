// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/floating, declared BitExactFixedDecomposition
// (docs/determinism.md §5): byte-identical at 1, 2, 3, 4 and 7 threads for the
// fixed decomposition, identical across repeats, and the decomposition itself
// is what the bits belong to.

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/floating/floating.hpp"
#include "unit/primitives/floating/executors.hpp"

namespace loadforge::primitives::floating {
namespace {

using mocks::ThreadedExecutor;

struct Outcome {
  std::uint64_t total_bits;
  std::vector<std::uint64_t> partial_bits;
  [[nodiscard]] bool operator==(const Outcome&) const = default;
};

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units) {
  ThreadedExecutor executor(threads);
  std::vector<double> partials(units);
  Outcome outcome{std::bit_cast<std::uint64_t>(run(executor, config, partials)), {}};
  for (const double partial : partials) {
    outcome.partial_bits.push_back(std::bit_cast<std::uint64_t>(partial));
  }
  return outcome;
}

TEST(FloatingDeterminism, ByteIdenticalAcrossThreadCounts) {
  const Config config{.seed = 0x5EED, .elements = 1237};
  const Outcome one = outcome_at(1, config, 19);
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    EXPECT_EQ(outcome_at(threads, config, 19), one) << threads << " threads";
  }
}

TEST(FloatingDeterminism, IdenticalAcrossRepeatRuns) {
  const Config config{.seed = 9, .elements = 500};
  const Outcome first = outcome_at(7, config, 13);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 13), first);
  }
}

TEST(FloatingDeterminism, ADifferentDecompositionMayLegitimatelyDiffer) {
  // The class promises identical bits for one decomposition, not across them:
  // the same units summed in another grouping give a different total.
  const Config config{.seed = 17, .elements = 4000};
  ThreadedExecutor executor(3);
  std::vector<double> partials(64);
  const double in_order = run(executor, config, partials);
  double reversed = 0.0;
  for (std::size_t i = partials.size(); i-- > 0;) {
    reversed += partials[i];
  }
  EXPECT_NEAR(in_order, reversed, 1e-9 * in_order);
}

}  // namespace
}  // namespace loadforge::primitives::floating
