// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/integer: properties over many generated cases. The
// cases come from the project's own counter-based generator, so a failure is
// reproduced by its case number.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "primitives/integer/integer.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/integer/executors.hpp"

namespace loadforge::primitives::integer {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 200;

Config case_config(std::uint64_t index) {
  const rng::Block draw = rng::block_at(0xC0FFEE, 1, index);
  return Config{.seed = rng::u64_of(draw), .steps = draw[2] % 300};
}

TEST(IntegerProperties, OneMoreStepChangesTheDigest) {
  // The step is a bijection on 64-bit values, so every lane moves; the folded
  // digest staying put would mean the lanes were not advanced.
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const Config config = case_config(index);
    const Config longer{.seed = config.seed, .steps = config.steps + 1};
    EXPECT_NE(unit_digest(config, index), unit_digest(longer, index)) << "case " << index;
  }
}

TEST(IntegerProperties, TheRunsSumIsIndependentOfPartitionAndOrder) {
  for (std::uint64_t index = 0; index < kCases / 4; ++index) {
    const Config config = case_config(index);
    const std::size_t units = 1 + (index % 13);
    SerialExecutor serial;
    ThreadedExecutor threaded(1 + (index % 5));
    std::vector<std::uint64_t> a(units);
    std::vector<std::uint64_t> b(units);
    const std::uint64_t sum_a = run(serial, config, a);
    const std::uint64_t sum_b = run(threaded, config, b);
    EXPECT_EQ(sum_a, sum_b) << "case " << index;
    EXPECT_EQ(a, b);
    std::reverse(b.begin(), b.end());
    EXPECT_EQ(std::accumulate(b.begin(), b.end(), std::uint64_t{0}), sum_a)
        << "wrapping addition does not care about order (case " << index << ")";
  }
}

TEST(IntegerProperties, UnitsAreIndependentOfTheUnitCount) {
  // Unit i's digest does not depend on how many units run beside it.
  const Config config{.seed = 5, .steps = 40};
  SerialExecutor executor;
  std::vector<std::uint64_t> small(3);
  std::vector<std::uint64_t> large(11);
  (void)run(executor, config, small);
  (void)run(executor, config, large);
  EXPECT_TRUE(std::equal(small.begin(), small.end(), large.begin()));
}

}  // namespace
}  // namespace loadforge::primitives::integer
