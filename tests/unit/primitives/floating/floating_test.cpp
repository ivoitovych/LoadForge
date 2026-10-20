// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/floating: the fixed decomposition replayed lane by
// lane, an exact integer oracle that bounds the rounding, and the executor
// contract run against this module's mock.

#include "primitives/floating/floating.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/floating/executors.hpp"

namespace loadforge::contracts {
using FloatingMockExecutors = ::testing::Types<primitives::floating::mocks::SerialSubject,
                                               primitives::floating::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(FloatingMocks, ExecutorContract, FloatingMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::floating {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

__extension__ using U128 = unsigned __int128;

struct Pair {
  std::uint64_t x_bits;  // the 53 significant bits: x = x_bits * 2^-53
  std::uint64_t y_bits;
};

std::uint64_t join(std::uint32_t low, std::uint32_t high) {
  return std::uint64_t{low} | (std::uint64_t{high} << 32U);
}

Pair pair_at(const Config& config, std::uint64_t unit, std::uint64_t element) {
  const rng::Block b = rng::block_at(config.seed, unit, element);
  return Pair{join(b[0], b[1]) >> 11U, join(b[2], b[3]) >> 11U};
}

/// The decomposition replayed accumulator by accumulator, from the definition
/// in the header rather than from the kernel's single pass.
double replay(const Config& config, std::uint64_t unit) {
  double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t lane = 0; lane < 8; ++lane) {
    for (std::uint64_t element = lane; element < config.elements; element += 8) {
      const Pair p = pair_at(config, unit, element);
      acc[lane] = std::fma(std::ldexp(static_cast<double>(p.x_bits), -53),
                           std::ldexp(static_cast<double>(p.y_bits), -53), acc[lane]);
    }
  }
  const double left = (acc[0] + acc[1]) + (acc[2] + acc[3]);
  const double right = (acc[4] + acc[5]) + (acc[6] + acc[7]);
  return left + right;
}

/// The exact sum of x*y over the unit's elements, which is an integer multiple
/// of 2^-106 held in 128 bits, rounded once to a double.
double exact(const Config& config, std::uint64_t unit) {
  U128 sum = 0;
  for (std::uint64_t element = 0; element < config.elements; ++element) {
    const Pair p = pair_at(config, unit, element);
    sum += static_cast<U128>(p.x_bits) * p.y_bits;
  }
  return std::ldexp(static_cast<double>(sum), -106);
}

constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;  // 2^-53

TEST(FloatingPrimitive, IsDeclaredBitExactFixedDecomposition) {
  EXPECT_EQ(kDeterminism, workloads::DeterminismClass::kBitExactFixedDecomposition);
}

TEST(FloatingPrimitive, NoElementsGivesPositiveZero) {
  const double value = unit_value(Config{.seed = 1, .elements = 0}, 0);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(value), 0U);
}

TEST(FloatingPrimitive, OneElementIsTheRoundedProduct) {
  const Config config{.seed = 3, .elements = 1};
  const Pair p = pair_at(config, 4, 0);
  const double x = std::ldexp(static_cast<double>(p.x_bits), -53);
  const double y = std::ldexp(static_cast<double>(p.y_bits), -53);
  EXPECT_EQ(unit_value(config, 4), x * y);
}

TEST(FloatingPrimitive, MatchesTheLaneReplayBitForBit) {
  // Lengths either side of every multiple of the accumulator count.
  for (const std::uint64_t elements : {1ULL, 7ULL, 8ULL, 9ULL, 15ULL, 16ULL, 17ULL, 100ULL, 1000ULL}) {
    const Config config{.seed = 0xFEEDFACE, .elements = elements};
    for (const std::uint64_t unit : {0ULL, 5ULL, 1ULL << 33U}) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(unit_value(config, unit)),
                std::bit_cast<std::uint64_t>(replay(config, unit)))
          << elements << " elements, unit " << unit;
    }
  }
}

TEST(FloatingPrimitive, StaysWithinTheRoundingBoundOfTheExactSum) {
  for (const std::uint64_t elements : {1ULL, 8ULL, 64ULL, 1000ULL, 20000ULL}) {
    const Config config{.seed = 11, .elements = elements};
    const double want = exact(config, 2);
    const double got = unit_value(config, 2);
    // Every term is positive, so the error is at most (n + 16) u of the sum:
    // n roundings along the chains, a few in the tree, one in the oracle.
    EXPECT_LE(std::fabs(got - want), static_cast<double>(elements + 16) * kUnitRoundoff * want)
        << elements << " elements";
  }
}

TEST(FloatingPrimitive, TheSumIsInTheRangeTheDataImplies) {
  // x and y are in [0, 1), so the sum of n products is in [0, n).
  const Config config{.seed = 8, .elements = 5000};
  const double value = unit_value(config, 0);
  EXPECT_GE(value, 0.0);
  EXPECT_LT(value, 5000.0);
  EXPECT_GT(value, 5000.0 * 0.15);  // the mean product is 1/4
}

TEST(FloatingPrimitive, EmptyRunReturnsPositiveZero) {
  SerialExecutor executor;
  std::vector<double> none;
  const double total = run(executor, Config{.seed = 1, .elements = 10}, none);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(total), 0U);
}

TEST(FloatingPrimitive, RunSumsTheUnitsLeftToRight) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 21, .elements = 333};
  std::vector<double> partials(9, 0.0);
  const double total = run(executor, config, partials);
  double expected = 0.0;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    EXPECT_EQ(partials[unit], replay(config, unit)) << "unit " << unit;
    expected += partials[unit];
  }
  EXPECT_EQ(std::bit_cast<std::uint64_t>(total), std::bit_cast<std::uint64_t>(expected));
}

}  // namespace
}  // namespace loadforge::primitives::floating
