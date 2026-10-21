// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the Monte Carlo oracle: the contract suite against it, the refusals
// at creation, and what each verdict says, from the specification's words.

#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "monte_carlo_units.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::tests {
namespace {

namespace spec = workloads::monte_carlo;
using contracts::OracleCase;
using verification::Agreement;
using verification::BoundedMismatch;
using verification::OracleFault;
using verification::Verdict;

workloads::Plan mc_plan(std::uint32_t chunk = spec::kDefaultChunkLength) {
  return workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, chunk, 0, 0};
}

std::vector<OracleCase> good_units() {
  return {mc_units::unit_of(0, 64), mc_units::unit_of(17, 256), mc_units::unit_of(50, 1024),
          mc_units::unit_of(63, 4096), mc_units::unit_of(5, 16384)};
}

/// Wrong in every batch, so whichever four the unit samples are caught.
std::vector<OracleCase> wrong_units() {
  std::vector<OracleCase> wrong;
  for (const double factor : {1.0 + 0x1p-30, 0.5, 2.0, -1.0, 0.0}) {
    OracleCase unit = mc_units::unit_of(3, 1024);
    for (std::size_t batch = 0; batch < spec::kBatches; ++batch) {
      mc_units::set_output(unit, batch, mc_units::output_of(unit, batch) * factor);
    }
    wrong.push_back(unit);
  }
  OracleCase nan = mc_units::unit_of(9, 1024);
  OracleCase inf = mc_units::unit_of(10, 1024);
  for (std::size_t batch = 0; batch < spec::kBatches; ++batch) {
    mc_units::set_output(nan, batch, std::numeric_limits<double>::quiet_NaN());
    mc_units::set_output(inf, batch, std::numeric_limits<double>::infinity());
  }
  wrong.push_back(nan);
  wrong.push_back(inf);
  return wrong;
}

struct MonteCarloSubject {
  static constexpr const char* kWorkload = "monte_carlo";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() {
    return verification::oracle::make_monte_carlo_oracle({}, mc_plan()).value();
  }
  static std::vector<OracleCase> good() { return good_units(); }
  static std::vector<OracleCase> wrong() { return wrong_units(); }
};

}  // namespace
}  // namespace loadforge::tests

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(MonteCarlo, OracleContract,
                               ::testing::Types<tests::MonteCarloSubject>);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

Verdict check(const verification::Oracle& oracle, const OracleCase& unit) {
  return contracts::oracle_suite::check(oracle, unit);
}

TEST(MonteCarloOracleCreate, EveryChunkLengthIsAccepted) {
  for (const std::uint32_t chunk : spec::kChunkLengths) {
    workloads::WorkloadConfig config;
    config.decomposition = chunk;
    const auto created = verification::oracle::make_monte_carlo_oracle(config, mc_plan(chunk));
    ASSERT_TRUE(created.has_value()) << chunk;
    EXPECT_EQ(created.value()->workload(), "monte_carlo");
    EXPECT_EQ(created.value()->contract(), verification::Contract::kBounded);
  }
}

TEST(MonteCarloOracleCreate, ADecompositionOfZeroIsTheDefaultAndIsAccepted) {
  EXPECT_TRUE(verification::oracle::make_monte_carlo_oracle({}, mc_plan()).has_value());
}

TEST(MonteCarloOracleCreate, AConfiguredDecompositionThatIsNotAChunkLengthIsRefused) {
  for (const std::uint32_t bad : {1U, 65U, 2048U, 32768U}) {
    workloads::WorkloadConfig config;
    config.decomposition = bad;
    const auto created = verification::oracle::make_monte_carlo_oracle(config, mc_plan());
    ASSERT_FALSE(created.has_value()) << bad;
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(bad)), std::string::npos);
  }
}

TEST(MonteCarloOracleCreate, APlanWhoseDecompositionIsNotAChunkLengthIsRefused) {
  for (const std::uint32_t bad : {0U, 65U, 32768U}) {
    const auto created = verification::oracle::make_monte_carlo_oracle({}, mc_plan(bad));
    ASSERT_FALSE(created.has_value()) << bad;
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_NE(created.error().detail.find("plan chunk length"), std::string::npos);
  }
}

TEST(MonteCarloOracleCreate, APlanWhoseSizesAreNotAMonteCarloUnitsIsRefused) {
  workloads::Plan plan = mc_plan();
  plan.input_bytes += 8;
  auto created = verification::oracle::make_monte_carlo_oracle({}, plan);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
  EXPECT_NE(created.error().detail.find("plan sizes"), std::string::npos);
  plan = mc_plan();
  plan.output_bytes -= 8;
  created = verification::oracle::make_monte_carlo_oracle({}, plan);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
}

TEST(MonteCarloOracleReplay, TheReplayedSumMatchesAnExactSumFromAnotherTool) {
  std::ifstream fixture(std::string(LOADFORGE_SOURCE_DIR) +
                        "/tests/fixtures/oracle/monte_carlo/exact_sums.txt");
  ASSERT_TRUE(fixture.is_open());
  std::string line;
  int rows = 0;
  while (std::getline(fixture, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::uint64_t key = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
    const double exact = std::strtod(line.substr(17).c_str(), nullptr);
    const double replayed = verification::oracle::replay_batch_sum(key);
    // Both are within an ulp of the exact sum, which fsum rounds correctly.
    EXPECT_LE(std::abs(replayed - exact), std::ldexp(exact, -52)) << line;
    ++rows;
  }
  EXPECT_EQ(rows, 6);
}

TEST(MonteCarloOracleReplay, EveryChunkLengthsSumIsWithinTheDerivedToleranceOfTheReplay) {
  for (const std::size_t chunk : spec::kChunkLengths) {
    for (const std::size_t batch : {0U, 31U, 63U}) {
      const double reference = verification::oracle::replay_batch_sum(spec::batch_key(1, 0, batch));
      const double chunked = mc_units::chunked_sum(mc_units::cached_terms(batch), chunk);
      EXPECT_LE(std::abs(chunked - reference), spec::tolerance(reference)) << chunk << " " << batch;
    }
  }
}

TEST(MonteCarloOracleVerdicts, AnAgreementReplayedTheSampledBatches) {
  const auto oracle = MonteCarloSubject::make();
  for (const OracleCase& unit : good_units()) {
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
    EXPECT_EQ(verification::blocks_of(verdict), spec::kSampledBatches);
  }
}

TEST(MonteCarloOracleVerdicts, AMismatchNamesTheBatchItsResidualAndItsTolerance) {
  const auto oracle = MonteCarloSubject::make();
  OracleCase unit = mc_units::unit_of(4, 1024);
  const std::size_t batch = spec::sampled_batch(4, 2);
  const double reference = verification::oracle::replay_batch_sum(mc_units::key_of(unit, batch));
  const double wrong = reference + 1.0;
  mc_units::set_output(unit, batch, wrong);
  const Verdict verdict = check(*oracle, unit);
  const auto* mismatch = std::get_if<BoundedMismatch>(&verdict);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(mismatch->check, "batch " + std::to_string(batch) + " sum");
  EXPECT_EQ(mismatch->residual, std::abs(wrong - reference));
  EXPECT_EQ(mismatch->tolerance, spec::tolerance(reference));
  EXPECT_FALSE(mismatch->condition_number.has_value());
  ASSERT_TRUE(mismatch->element.has_value());
  EXPECT_EQ(*mismatch->element, batch);
  EXPECT_EQ(mismatch->blocks_compared, spec::kSampledBatches);
  EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kBounded));
}

TEST(MonteCarloOracleVerdicts, TheSmallestFailingBatchIsNamedNotTheFirstReplayed) {
  const auto oracle = MonteCarloSubject::make();
  // Unit 50 samples 50, 2, 18, 34, in that order: 2 is the smallest, and
  // replayed second. Unit 0 samples 0, 16, 32, 48: with 16 and 48 wrong the
  // first replayed of them is the smallest.
  struct Row {
    std::uint64_t index;
    std::size_t first;
    std::size_t second;
    std::size_t named;
  };
  for (const Row row : {Row{50, 50, 2, 2}, Row{0, 16, 48, 16}}) {
    OracleCase unit = mc_units::unit_of(row.index, 1024);
    for (const std::size_t batch : {row.first, row.second}) {
      mc_units::set_output(unit, batch, mc_units::output_of(unit, batch) * 2.0);
    }
    const Verdict verdict = check(*oracle, unit);
    const auto& mismatch = std::get<BoundedMismatch>(verdict);
    EXPECT_EQ(*mismatch.element, row.named) << row.index;
    EXPECT_EQ(mismatch.check, "batch " + std::to_string(row.named) + " sum");
    EXPECT_EQ(mismatch.blocks_compared, spec::kSampledBatches);
  }
}

TEST(MonteCarloOracleVerdicts, ABatchTheUnitDoesNotSampleIsNotChecked) {
  const auto oracle = MonteCarloSubject::make();
  OracleCase unit = mc_units::unit_of(0, 1024);  // samples 0, 16, 32, 48
  mc_units::set_output(unit, 1, 0.0);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(*oracle, unit)));
}

}  // namespace
}  // namespace loadforge::tests
