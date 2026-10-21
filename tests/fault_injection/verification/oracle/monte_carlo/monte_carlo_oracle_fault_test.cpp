// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the Monte Carlo oracle: every corruption of a unit's output
// or input is caught or, where the specification's derived bound lets it
// through, shown to be let through exactly at that bound; and every way the
// check can fail to happen is NotChecked, never an Agreement.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_case.hpp"
#include "monte_carlo_units.hpp"
#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::contracts {
// The contract header registers both suites; this binary runs neither.
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

namespace spec = workloads::monte_carlo;
using contracts::OracleCase;
using verification::Agreement;
using verification::BoundedMismatch;
using verification::NotChecked;
using verification::Verdict;

std::unique_ptr<verification::Oracle> make_oracle() {
  const workloads::Plan plan{
      spec::kInputBytes, spec::kOutputBytes, 0, 1, spec::kDefaultChunkLength, 0, 0};
  return verification::oracle::make_monte_carlo_oracle({}, plan).value();
}

Verdict check(const verification::Oracle& oracle, const OracleCase& unit) {
  return contracts::oracle_suite::check(oracle, unit);
}

// Unit 0 samples batches 0, 16, 32 and 48.
constexpr std::size_t kBatch = 0;

// ---- T5: every bit of a sampled sum ----------------------------------------------

TEST(MonteCarloOracleFaults, EveryBitOfASampledSumFlippedIsCaughtExceptTheOnesBelowTheBound) {
  const auto oracle = make_oracle();
  const OracleCase right = mc_units::unit_of(0, 1024);
  // The sums are near 12867 = 2^13.65: an ulp is 2^-39, the tolerance about
  // 2^-24.35, so a flip of mantissa bit k moves the sum by 2^(k-39), within
  // the tolerance for k <= 14 and beyond it from k = 15. Every exponent and
  // sign bit moves it by far more.
  for (std::size_t bit = 0; bit < 64; ++bit) {
    OracleCase unit = right;
    mc_units::put64(unit.output, kBatch * 8,
                    mc_units::get64(right.output, kBatch * 8) ^ (std::uint64_t{1} << bit));
    const Verdict verdict = check(*oracle, unit);
    if (bit <= 14) {
      EXPECT_TRUE(std::holds_alternative<Agreement>(verdict)) << bit;
    } else {
      ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << bit;
      EXPECT_EQ(*std::get<BoundedMismatch>(verdict).element, kBatch) << bit;
      EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded)) << bit;
    }
  }
}

TEST(MonteCarloOracleFaults, NonFiniteZeroAndNegativeSumsAreFailuresWithTheirResiduals) {
  const auto oracle = make_oracle();
  const std::vector<double> values = {std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(),
                                      0.0,
                                      -0.0,
                                      std::numeric_limits<double>::denorm_min()};
  for (const double value : values) {
    OracleCase unit = mc_units::unit_of(0, 1024);
    mc_units::set_output(unit, kBatch, value);
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << value;
    const auto& mismatch = std::get<BoundedMismatch>(verdict);
    EXPECT_TRUE(std::isnan(value) ? std::isnan(mismatch.residual)
                                  : mismatch.residual > mismatch.tolerance)
        << value;
    EXPECT_TRUE(std::isfinite(mismatch.tolerance));
    EXPECT_GT(mismatch.tolerance, 0.0);
    EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded)) << value;
  }
}

// ---- T5: the bound, held and breached --------------------------------------------

/// The largest double whose distance from `reference` is at most `bound`, on
/// the high side (`up`) or the low side.
double furthest_within(double reference, double bound, bool up) {
  const double toward =
      up ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
  double value = reference + (up ? bound : -bound);
  while (std::abs(value - reference) > bound) {
    value = std::nextafter(value, -toward);
  }
  while (std::abs(std::nextafter(value, toward) - reference) <= bound) {
    value = std::nextafter(value, toward);
  }
  return value;
}

TEST(MonteCarloOracleFaults, TheBoundIsHeldAtItsEdgeAndBreachedOneUlpBeyondOnBothSides) {
  const auto oracle = make_oracle();
  for (const bool up : {true, false}) {
    for (const std::size_t batch : {0U, 16U}) {
      OracleCase unit = mc_units::unit_of(0, 1024);
      const double reference =
          verification::oracle::replay_batch_sum(mc_units::key_of(unit, batch));
      const double bound = spec::tolerance(reference);
      const double held = furthest_within(reference, bound, up);
      mc_units::set_output(unit, batch, held);
      EXPECT_TRUE(std::holds_alternative<Agreement>(check(*oracle, unit))) << up << batch;
      const double breached = std::nextafter(held, up ? std::numeric_limits<double>::infinity()
                                                      : -std::numeric_limits<double>::infinity());
      mc_units::set_output(unit, batch, breached);
      const Verdict verdict = check(*oracle, unit);
      ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << up << batch;
      const auto& mismatch = std::get<BoundedMismatch>(verdict);
      EXPECT_GT(mismatch.residual, mismatch.tolerance);
      EXPECT_EQ(mismatch.tolerance, bound);
      EXPECT_EQ(mismatch.residual, std::abs(breached - reference));
    }
  }
}

// ---- T5: which batches a unit looks at -------------------------------------------

TEST(MonteCarloOracleFaults, ABatchCorruptedIsCaughtInExactlyTheUnitsThatSampleIt) {
  const auto oracle = make_oracle();
  constexpr std::size_t kCorrupt = 5;
  OracleCase base = mc_units::unit_of(0, 1024);
  mc_units::set_output(base, kCorrupt, mc_units::output_of(base, kCorrupt) * 1.5);
  std::size_t caught = 0;
  for (std::uint64_t index = 0; index < 64; ++index) {
    OracleCase unit = base;
    unit.index = index;
    bool sampled = false;
    for (std::size_t m = 0; m < spec::kSampledBatches; ++m) {
      sampled = sampled || spec::sampled_batch(index, m) == kCorrupt;
    }
    const Verdict verdict = check(*oracle, unit);
    EXPECT_EQ(std::holds_alternative<BoundedMismatch>(verdict), sampled) << index;
    caught += std::holds_alternative<BoundedMismatch>(verdict) ? 1 : 0;
  }
  EXPECT_EQ(caught, 4U);  // 64 consecutive units sample every batch four times
}

// ---- T5: the input corrupted, before and after compute() -----------------------

TEST(MonteCarloOracleFaults, ABatchKeyCorruptedBeforeComputeAgreesAndAfterComputeIsCaught) {
  const auto oracle = make_oracle();
  const OracleCase unit = mc_units::unit_of(0, 1024);
  const std::uint64_t flipped = mc_units::key_of(unit, 16) ^ (std::uint64_t{1} << 40U);
  // Before: the workload read the flipped key and summed its samples.
  OracleCase before = unit;
  mc_units::put64(before.input, std::size_t{16} * 8, flipped);
  mc_units::set_output(before, 16, mc_units::chunked_sum(mc_units::terms_of(flipped), 1024));
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(*oracle, before)));
  // After: the output is the sum for the original key, the input now says otherwise.
  OracleCase after = unit;
  mc_units::put64(after.input, std::size_t{16} * 8, flipped);
  const Verdict verdict = check(*oracle, after);
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_EQ(*std::get<BoundedMismatch>(verdict).element, 16U);
}

// ---- T5b: a check that was not made --------------------------------------------

TEST(MonteCarloOracleUnrun, EvidenceOfTheWrongSizeIsNotCheckedWithNothingCompared) {
  const auto oracle = make_oracle();
  std::vector<OracleCase> units(6, mc_units::unit_of(0, 1024));
  units[0].output.pop_back();
  units[1].output.push_back(std::byte{0});
  units[2].output.clear();
  units[3].input.pop_back();
  units[4].input.push_back(std::byte{0});
  units[5].input.clear();
  for (const OracleCase& unit : units) {
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    EXPECT_EQ(verification::blocks_of(verdict), 0U);
    EXPECT_FALSE(std::get<NotChecked>(verdict).what.empty());
    EXPECT_FALSE(std::get<NotChecked>(verdict).why.empty());
    EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
  }
}

TEST(MonteCarloOracleUnrun, EveryBatchKeyIsInTheDomainSoNoInputIsNotChecked) {
  const auto oracle = make_oracle();
  for (const std::uint64_t key : {std::uint64_t{0}, ~std::uint64_t{0}, std::uint64_t{1} << 63U}) {
    OracleCase unit{3, std::vector<std::byte>(spec::kInputBytes),
                    std::vector<std::byte>(spec::kOutputBytes)};
    const double sum = mc_units::chunked_sum(mc_units::terms_of(key), 1024);
    for (std::size_t batch = 0; batch < spec::kBatches; ++batch) {
      mc_units::put64(unit.input, batch * 8, key);
      mc_units::set_output(unit, batch, sum);
    }
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << key;
    EXPECT_EQ(verification::blocks_of(verdict), spec::kSampledBatches);
  }
}

}  // namespace
}  // namespace loadforge::tests
