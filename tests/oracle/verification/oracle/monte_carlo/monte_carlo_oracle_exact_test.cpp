// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the Monte Carlo oracle: its replayed sum against the exact sum of the
// same integrand values, computed here in integers. Every value is a multiple
// of 2^-79 (it is at least 2^-26 and has 53 significant bits), so the sum of
// sixteen thousand of them is an integer count of 2^-79 that fits 128 bits,
// and the comparison with the oracle's double is exact.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "monte_carlo_units.hpp"
#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::contracts {
// The contract header registers both suites; this binary runs neither.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

namespace spec = workloads::monte_carlo;

__extension__ using U128 = unsigned __int128;

/// v / 2^-79, exactly: v is positive, finite and a multiple of 2^-79.
U128 in_units_of_2_pow_minus_79(double value) {
  int exponent = 0;
  const double fraction = std::frexp(value, &exponent);
  const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
  const int shift = exponent - 53 + 79;
  EXPECT_GE(shift, 0);
  return static_cast<U128>(mantissa) << static_cast<unsigned>(shift);
}

U128 exact_sum(const std::vector<double>& terms) {
  U128 total = 0;
  for (const double term : terms) {
    total += in_units_of_2_pow_minus_79(term);
  }
  return total;
}

TEST(MonteCarloOracleExact, TheReplayedSumIsWithinAnUlpOfTheExactSumOfTheSameValues) {
  const std::vector<std::uint64_t> keys = {0,
                                           ~std::uint64_t{0},
                                           std::uint64_t{1} << 63U,
                                           0x0123456789ABCDEFULL,
                                           spec::batch_key(1, 0, 0),
                                           spec::batch_key(2, 5, 17),
                                           spec::batch_key(3, 63, 63)};
  for (const std::uint64_t key : keys) {
    const U128 exact = exact_sum(mc_units::terms_of(key));
    const U128 replayed = in_units_of_2_pow_minus_79(verification::oracle::replay_batch_sum(key));
    const U128 distance = replayed > exact ? replayed - exact : exact - replayed;
    EXPECT_LE(distance, (exact >> 52U) + 1U) << key;
  }
}

TEST(MonteCarloOracleExact, EveryChunkedSumIsWithinTheDerivedToleranceOfTheExactSum) {
  for (const std::size_t chunk : spec::kChunkLengths) {
    const std::vector<double>& terms = mc_units::cached_terms(7);
    const U128 exact = exact_sum(terms);
    const U128 chunked = in_units_of_2_pow_minus_79(mc_units::chunked_sum(terms, chunk));
    const U128 distance = chunked > exact ? chunked - exact : exact - chunked;
    // tolerance(r) / 2: the workload may be that far from the exact sum, the
    // oracle's replay being nearly on it.
    EXPECT_LE(distance, exact >> 39U) << chunk;
  }
}

}  // namespace
}  // namespace loadforge::tests
