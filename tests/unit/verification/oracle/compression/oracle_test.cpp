// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compression oracle against OracleContract (ADR 0003): good units are
// the reference tool's blocks (tests/fixtures/lz4/), wrong units cover every
// rule the oracle checks. And what the contract leaves to this oracle: its
// name, its refusals of a plan that is not a compression unit's, its scratch.

#include "verification/oracle/compression/oracle.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "unit/verification/oracle/compression/lz4_support.hpp"
#include "verification/oracle.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::compression {

namespace spec = workloads::compression;
namespace lz4 = test::lz4;

workloads::Plan compression_plan() {
  return workloads::Plan{
      spec::kInputBytes, spec::output_bytes_for(spec::kInputBytes), 0, 4, 1, 2 * spec::kInputBytes,
      spec::kInputBytes};
}

struct CompressionOracleSubject {
  static constexpr const char* kWorkload = "compression";
  static constexpr Contract kContract = Contract::kExact;
  static std::unique_ptr<Oracle> make() {
    auto created = create(workloads::WorkloadConfig{}, compression_plan());
    return std::move(created).value();
  }
  static std::vector<contracts::OracleCase> good() {
    std::vector<contracts::OracleCase> cases;
    std::uint64_t index = 0;
    for (const lz4::Fixture& fixture : lz4::fixtures()) {
      if (fixture.kind == "unit") {
        cases.push_back(lz4::unit_case(index++, fixture.input, fixture.block));
      }
    }
    return cases;
  }
  static std::vector<contracts::OracleCase> wrong() {
    std::vector<contracts::OracleCase> cases;
    for (const lz4::WrongUnit& wrong : lz4::wrong_units()) {
      cases.push_back(wrong.unit);
    }
    return cases;
  }
};

namespace {

TEST(CompressionOracle, NamesTheCompressionWorkloadAndTheExactContract) {
  const CompressionOracle oracle;
  EXPECT_EQ(oracle.workload(), "compression");
  EXPECT_EQ(oracle.workload(), kWorkload);
  EXPECT_EQ(oracle.contract(), Contract::kExact);
}

TEST(CompressionOracle, EveryReferenceToolUnitIsAgreedWithAfterOneBlock) {
  const CompressionOracle oracle;
  std::size_t units = 0;
  for (const contracts::OracleCase& unit : CompressionOracleSubject::good()) {
    const Verdict verdict = oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "unit " << unit.index;
    EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, 1U);
    ++units;
  }
  EXPECT_EQ(units, 7U);
}

TEST(CompressionOracle, AUnitOnBothEndOfBlockMarginsIsAgreedWith) {
  // The periodic unit's match ends exactly five bytes before the end; this
  // one's starts exactly twelve before it.
  const CompressionOracle oracle;
  const lz4::Bytes input = lz4::periodic_input();
  for (const lz4::Bytes& block :
       {lz4::periodic_unit_block(), lz4::periodic_block(spec::kInputBytes - 12, 7)}) {
    const contracts::OracleCase unit = lz4::unit_case(9, input, block);
    const Verdict verdict = oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
    EXPECT_TRUE(std::holds_alternative<Agreement>(verdict));
  }
}

TEST(CompressionOracle, IsCreatedForTheCompressionPlan) {
  for (const std::uint32_t configured : {0U, 1U}) {
    workloads::WorkloadConfig config;
    config.decomposition = configured;
    const auto created = create(config, compression_plan());
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    EXPECT_EQ(created.value()->workload(), "compression");
    EXPECT_EQ(created.value()->contract(), Contract::kExact);
  }
}

void expect_refused(const workloads::WorkloadConfig& config, const workloads::Plan& plan,
                    const std::string& names) {
  const auto created = create(config, plan);
  ASSERT_FALSE(created.has_value()) << names;
  EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
  EXPECT_NE(created.error().detail.find(names), std::string::npos) << created.error().detail;
}

TEST(CompressionOracle, RefusesAPlanThatIsNotACompressionUnits) {
  const workloads::WorkloadConfig config;
  workloads::Plan plan = compression_plan();
  plan.input_bytes = spec::kInputBytes + 1;
  expect_refused(config, plan, "the plan says 65537 and 65817");
  plan = compression_plan();
  plan.output_bytes = spec::output_bytes_for(spec::kInputBytes) - 1;
  expect_refused(config, plan, "the plan says 65536 and 65816");
  plan = compression_plan();
  plan.decomposition = 2;
  expect_refused(config, plan, "not 2 and 0");
  workloads::WorkloadConfig two;
  two.decomposition = 2;
  expect_refused(two, compression_plan(), "not 1 and 2");
}

TEST(CompressionOracle, HeapScratchHoldsADecodedUnit) {
  const std::unique_ptr<Scratch> scratch = heap_scratch();
  ASSERT_NE(scratch, nullptr);
  EXPECT_EQ(scratch->size(), spec::kInputBytes);
  (*scratch)[0] = std::byte{1};
  scratch->back() = std::byte{2};
  EXPECT_EQ(scratch->back(), std::byte{2});
}

}  // namespace
}  // namespace loadforge::verification::oracle::compression

namespace loadforge::contracts {
namespace {
using CompressionOracleSubjects =
    ::testing::Types<verification::oracle::compression::CompressionOracleSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(CompressionOracle, OracleContract, CompressionOracleSubjects);
}  // namespace

// The registry's suite comes with the header; the registry is the
// integration stream's to provide (oracle.hpp), not this module's.
}  // namespace loadforge::contracts
