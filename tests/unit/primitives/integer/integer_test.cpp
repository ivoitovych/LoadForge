// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/integer: the invariants of the load, an oracle that
// does not step, and the executor contract run against this module's mock.

#include "primitives/integer/integer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/integer/executors.hpp"

namespace loadforge::contracts {
using IntegerMockExecutors = ::testing::Types<primitives::integer::mocks::SerialSubject,
                                              primitives::integer::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(IntegerMocks, ExecutorContract, IntegerMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::integer {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

/// The oracle: the n-fold composition of x -> A*x + C, found by squaring the
/// affine map instead of stepping it. Derived from the algebra, not from the
/// loop it checks: (a1,c1) then (a2,c2) is x -> a2*(a1*x + c1) + c2.
struct Affine {
  std::uint64_t a;
  std::uint64_t c;
};

Affine then(Affine first, Affine second) {
  return Affine{second.a * first.a, second.a * first.c + second.c};
}

Affine power(std::uint64_t steps) {
  Affine result{1, 0};
  Affine base{kMultiplier, kIncrement};
  for (; steps != 0; steps >>= 1U) {
    if ((steps & 1U) != 0) {
      result = then(result, base);
    }
    base = then(base, base);
  }
  return result;
}

std::uint64_t oracle_digest(const Config& config, std::uint64_t unit) {
  const Affine jump = power(config.steps);
  std::array<std::uint64_t, 4> end{};
  for (std::uint64_t lane = 0; lane < 4; ++lane) {
    const std::uint64_t start = rng::u64_of(rng::block_at(config.seed, unit, lane));
    end[lane] = jump.a * start + jump.c;
  }
  return end[0] ^ std::rotl(end[1], 16) ^ std::rotl(end[2], 32) ^ std::rotl(end[3], 48);
}

TEST(IntegerPrimitive, IsDeclaredBitExact) {
  EXPECT_EQ(kDeterminism, workloads::DeterminismClass::kBitExact);
}

TEST(IntegerPrimitive, ZeroStepsDigestIsTheFoldOfTheStart) {
  const Config config{.seed = 7, .steps = 0};
  EXPECT_EQ(unit_digest(config, 3), oracle_digest(config, 3));
}

TEST(IntegerPrimitive, OneStepIsOneMultiplyAndAdd) {
  const Config config{.seed = 7, .steps = 1};
  std::array<std::uint64_t, 4> lanes{};
  for (std::uint64_t lane = 0; lane < 4; ++lane) {
    lanes[lane] = rng::u64_of(rng::block_at(7, 5, lane)) * kMultiplier + kIncrement;
  }
  EXPECT_EQ(unit_digest(config, 5),
            lanes[0] ^ std::rotl(lanes[1], 16) ^ std::rotl(lanes[2], 32) ^ std::rotl(lanes[3], 48));
}

TEST(IntegerPrimitive, MatchesTheJumpAheadOracleAtManyLengths) {
  for (const std::uint64_t steps :
       {0ULL, 1ULL, 2ULL, 3ULL, 63ULL, 64ULL, 65ULL, 1000ULL, 4097ULL}) {
    for (const std::uint64_t unit : {0ULL, 1ULL, 0xFFFFFFFFULL, 0x100000000ULL}) {
      const Config config{.seed = 0x0123456789ABCDEFULL, .steps = steps};
      EXPECT_EQ(unit_digest(config, unit), oracle_digest(config, unit))
          << "steps " << steps << " unit " << unit;
    }
  }
}

TEST(IntegerPrimitive, DifferentSeedsAndUnitsGiveDifferentDigests) {
  const Config a{.seed = 1, .steps = 100};
  const Config b{.seed = 2, .steps = 100};
  EXPECT_NE(unit_digest(a, 0), unit_digest(b, 0));
  EXPECT_NE(unit_digest(a, 0), unit_digest(a, 1));
}

TEST(IntegerPrimitive, EmptyRunRunsNothingAndReturnsZero) {
  SerialExecutor executor;
  std::vector<std::uint64_t> none;
  EXPECT_EQ(run(executor, Config{.seed = 1, .steps = 10}, none), 0U);
}

TEST(IntegerPrimitive, RunWritesEachUnitsDigestAndSumsThem) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 99, .steps = 500};
  std::vector<std::uint64_t> partials(10, 0);
  const std::uint64_t total = run(executor, config, partials);
  std::uint64_t expected = 0;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    EXPECT_EQ(partials[unit], oracle_digest(config, unit)) << "unit " << unit;
    expected += oracle_digest(config, unit);
  }
  EXPECT_EQ(total, expected);
}

TEST(IntegerPrimitive, SumWrapsModuloTwoToTheSixtyFourWithoutUndefinedBehaviour) {
  // Digests are full-range, so a sum of a few wraps; UBSan (CI) would flag a
  // signed or checked operation here.
  SerialExecutor executor;
  const Config config{.seed = 0xFFFFFFFFFFFFFFFFULL, .steps = 3};
  std::vector<std::uint64_t> partials(64, 0);
  std::uint64_t expected = 0;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    expected += oracle_digest(config, unit);
  }
  EXPECT_EQ(run(executor, config, partials), expected);
}

}  // namespace
}  // namespace loadforge::primitives::integer
