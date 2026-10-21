// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the sort oracle: the contract suite against it, the refusals at
// creation, and what each verdict says, from the specification's own words.

#include "verification/oracle/sort/sort_oracle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "sort_units.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::tests {
namespace {

namespace spec = workloads::sort;
using contracts::OracleCase;
using verification::Agreement;
using verification::ExactMismatch;
using verification::OracleFault;
using verification::Verdict;

workloads::Plan sort_plan(std::uint32_t chunks = spec::kDefaultChunkCount) {
  return workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, chunks, 0, 0};
}

std::vector<OracleCase> wrong_units() {
  const std::vector<OracleCase> good = sort_units::good_units();
  std::vector<OracleCase> wrong;
  OracleCase bit = good[0];
  bit.output[sort_units::spec::key_offset(100) + 3] ^= std::byte{0x10};
  wrong.push_back(bit);
  OracleCase range = good[0];
  range.output[spec::index_offset(5) + 3] ^= std::byte{0x80};
  wrong.push_back(range);
  OracleCase repeat = good[0];
  sort_units::put32(repeat.output, spec::index_offset(9),
                    sort_units::get32(repeat.output, spec::index_offset(8)));
  wrong.push_back(repeat);
  OracleCase swapped = good[3];
  for (std::size_t byte = 0; byte < 8; ++byte) {
    std::swap(swapped.output[spec::key_offset(20) + byte],
              swapped.output[spec::key_offset(21) + byte]);
  }
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(swapped.output[spec::index_offset(20) + byte],
              swapped.output[spec::index_offset(21) + byte]);
  }
  wrong.push_back(swapped);
  OracleCase tie = good[1];
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(tie.output[spec::index_offset(0) + byte], tie.output[spec::index_offset(1) + byte]);
  }
  wrong.push_back(tie);
  return wrong;
}

struct SortSubject {
  static constexpr const char* kWorkload = "sort";
  static constexpr verification::Contract kContract = verification::Contract::kExact;
  static std::unique_ptr<verification::Oracle> make() {
    return verification::oracle::make_sort_oracle({}, sort_plan()).value();
  }
  static std::vector<OracleCase> good() { return sort_units::good_units(); }
  static std::vector<OracleCase> wrong() { return wrong_units(); }
};

}  // namespace
}  // namespace loadforge::tests

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Sort, OracleContract, ::testing::Types<tests::SortSubject>);
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

Verdict check(const verification::Oracle& oracle, const OracleCase& unit) {
  return contracts::oracle_suite::check(oracle, unit);
}

TEST(SortOracleCreate, EveryChunkCountIsAccepted) {
  for (const std::uint32_t chunks : spec::kChunkCounts) {
    workloads::WorkloadConfig config;
    config.decomposition = chunks;
    const auto created = verification::oracle::make_sort_oracle(config, sort_plan(chunks));
    ASSERT_TRUE(created.has_value()) << chunks;
    EXPECT_EQ(created.value()->workload(), "sort");
    EXPECT_EQ(created.value()->contract(), verification::Contract::kExact);
  }
}

TEST(SortOracleCreate, ADecompositionOfZeroIsTheDefaultAndIsAccepted) {
  const auto created = verification::oracle::make_sort_oracle({}, sort_plan());
  EXPECT_TRUE(created.has_value());
}

TEST(SortOracleCreate, AConfiguredDecompositionThatIsNotAChunkCountIsRefused) {
  for (const std::uint32_t bad : {3U, 12U, 65U, 128U}) {
    workloads::WorkloadConfig config;
    config.decomposition = bad;
    const auto created = verification::oracle::make_sort_oracle(config, sort_plan());
    ASSERT_FALSE(created.has_value()) << bad;
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(bad)), std::string::npos);
  }
}

TEST(SortOracleCreate, APlanWhoseDecompositionIsNotAChunkCountIsRefused) {
  for (const std::uint32_t bad : {0U, 3U, 128U}) {
    const auto created = verification::oracle::make_sort_oracle({}, sort_plan(bad));
    ASSERT_FALSE(created.has_value()) << bad;
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_NE(created.error().detail.find("plan chunk count"), std::string::npos);
  }
}

TEST(SortOracleCreate, APlanWhoseSizesAreNotASortUnitsIsRefused) {
  workloads::Plan plan = sort_plan();
  plan.input_bytes -= 8;
  auto created = verification::oracle::make_sort_oracle({}, plan);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
  EXPECT_NE(created.error().detail.find("plan sizes"), std::string::npos);
  plan = sort_plan();
  plan.output_bytes += 12;
  created = verification::oracle::make_sort_oracle({}, plan);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
}

TEST(SortOracleVerdicts, AnAgreementComparedEveryKey) {
  const auto oracle = SortSubject::make();
  for (const OracleCase& unit : SortSubject::good()) {
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
    EXPECT_EQ(verification::blocks_of(verdict), spec::kKeys);
  }
}

TEST(SortOracleVerdicts, AnIndexOutOfRangeIsNamedAtItsPositionWithoutADifference) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  sort_units::put32(unit.output, spec::index_offset(77), static_cast<std::uint32_t>(spec::kKeys));
  sort_units::put32(unit.output, spec::index_offset(900), 0xFFFFFFFFU);
  const Verdict verdict = check(*oracle, unit);
  const auto* mismatch = std::get_if<ExactMismatch>(&verdict);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(mismatch->check, "index out of range at position 77");
  EXPECT_FALSE(mismatch->difference.has_value());
  EXPECT_EQ(mismatch->blocks_compared, 78U);
}

TEST(SortOracleVerdicts, ARepeatedIndexIsNamedAtTheSecondPosition) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  sort_units::put32(unit.output, spec::index_offset(1000),
                    sort_units::get32(unit.output, spec::index_offset(3)));
  const Verdict verdict = check(*oracle, unit);
  const auto* mismatch = std::get_if<ExactMismatch>(&verdict);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(mismatch->check, "index repeated at position 1000");
  EXPECT_FALSE(mismatch->difference.has_value());
  EXPECT_EQ(mismatch->blocks_compared, 1001U);
}

TEST(SortOracleVerdicts, TheIndicesAreJudgedBeforeTheKeysAndTheKeysBeforeTheOrder) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  // A key wrong at position 2 and an index repeated at position 50: step 1 wins.
  unit.output[spec::key_offset(2)] ^= std::byte{1};
  sort_units::put32(unit.output, spec::index_offset(50),
                    sort_units::get32(unit.output, spec::index_offset(49)));
  Verdict verdict = check(*oracle, unit);
  EXPECT_EQ(std::get<ExactMismatch>(verdict).check, "index repeated at position 50");
  // Restore the index; a swapped pair at 60 and 61 is a permutation with the
  // right keys, but a key wrong at 2 is judged first.
  unit = sort_units::good_units()[0];
  unit.output[spec::key_offset(2)] ^= std::byte{1};
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(unit.output[spec::index_offset(60) + byte],
              unit.output[spec::index_offset(61) + byte]);
  }
  verdict = check(*oracle, unit);
  EXPECT_NE(std::get<ExactMismatch>(verdict).check.find("key differs"), std::string::npos);
}

TEST(SortOracleVerdicts, AWrongKeyNamesTheInputKeyItsIndexNamesAndItsDifference) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  const std::uint32_t named = sort_units::get32(unit.output, spec::index_offset(100));
  unit.output[spec::key_offset(100) + 3] ^= std::byte{0x10};
  const Verdict verdict = check(*oracle, unit);
  const auto* mismatch = std::get_if<ExactMismatch>(&verdict);
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(mismatch->check,
            "key differs from input key " + std::to_string(named) + " at position 100");
  EXPECT_EQ(mismatch->blocks_compared, spec::kKeys);
  ASSERT_TRUE(mismatch->difference.has_value());
  const verification::Difference& difference = *mismatch->difference;
  EXPECT_EQ(difference.domain, verification::Domain::kStored);
  EXPECT_EQ(difference.first_offset, 803U);
  EXPECT_EQ(difference.last_offset, 803U);
  EXPECT_EQ(difference.popcount, 1U);
  EXPECT_EQ(difference.window_offset, 800U);
  ASSERT_EQ(difference.expected.size(), 64U);
  ASSERT_EQ(difference.actual.size(), 64U);
  // The window is output bytes 800 .. 863: keys 100 .. 107, from byte 0 of 100.
  for (std::size_t at = 0; at < 64; ++at) {
    EXPECT_EQ(difference.actual[at], unit.output[800 + at]);
    const std::uint32_t index = sort_units::get32(unit.output, spec::index_offset(100 + at / 8));
    EXPECT_EQ(difference.expected[at], unit.input[std::size_t{index} * 8 + at % 8]);
  }
  EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kExact));
}

TEST(SortOracleVerdicts, TheDifferenceSpansEveryWrongKeyNotJustTheFirst) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  unit.output[spec::key_offset(10) + 1] ^= std::byte{0x03};    // 2 bits, byte 81
  unit.output[spec::key_offset(5000) + 7] ^= std::byte{0x80};  // 1 bit, byte 40007
  unit.output[spec::key_offset(5000) + 2] ^= std::byte{0xFF};  // 8 bits, byte 40002
  const Verdict verdict = check(*oracle, unit);
  const auto& mismatch = std::get<ExactMismatch>(verdict);
  ASSERT_TRUE(mismatch.difference.has_value());
  EXPECT_EQ(mismatch.difference->first_offset, 81U);
  EXPECT_EQ(mismatch.difference->last_offset, 40007U);
  EXPECT_EQ(mismatch.difference->popcount, 11U);
  EXPECT_EQ(mismatch.difference->window_offset, 80U);
  EXPECT_NE(mismatch.check.find("at position 10"), std::string::npos);
}

TEST(SortOracleVerdicts, TheWindowStopsAtTheEndOfTheKeysRegion) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[0];
  unit.output[spec::key_offset(spec::kKeys - 1) + 6] ^= std::byte{0x01};
  const Verdict verdict = check(*oracle, unit);
  const auto& difference = *std::get<ExactMismatch>(verdict).difference;
  EXPECT_EQ(difference.first_offset, spec::kIndexRegionOffset - 2);
  EXPECT_EQ(difference.window_offset, spec::kIndexRegionOffset - 8);
  EXPECT_EQ(difference.expected.size(), 8U);
  EXPECT_TRUE(verification::is_valid(difference));
}

TEST(SortOracleVerdicts, AMisorderedPermutationIsNamedAtTheFirstPositionOutOfOrder) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[3];
  for (std::size_t byte = 0; byte < 8; ++byte) {
    std::swap(unit.output[spec::key_offset(20) + byte], unit.output[spec::key_offset(21) + byte]);
  }
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(unit.output[spec::index_offset(20) + byte],
              unit.output[spec::index_offset(21) + byte]);
  }
  const Verdict verdict = check(*oracle, unit);
  const auto& mismatch = std::get<ExactMismatch>(verdict);
  EXPECT_EQ(mismatch.check, "order broken at position 21");
  EXPECT_FALSE(mismatch.difference.has_value());
  EXPECT_EQ(mismatch.blocks_compared, spec::kKeys);
}

TEST(SortOracleVerdicts, EqualKeysInTheWrongPositionOrderAreMisordered) {
  const auto oracle = SortSubject::make();
  OracleCase unit = sort_units::good_units()[1];
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(unit.output[spec::index_offset(0) + byte], unit.output[spec::index_offset(1) + byte]);
  }
  const Verdict verdict = check(*oracle, unit);
  EXPECT_EQ(std::get<ExactMismatch>(verdict).check, "order broken at position 1");
}

TEST(SortOracleVerdicts, KeysAreComparedAsUnsignedIntegers) {
  const auto oracle = SortSubject::make();
  // The key at the sign bit sorts after the one below it: the other order is wrong.
  std::vector<std::uint64_t> keys(spec::kKeys, 0x7FFFFFFFFFFFFFFFULL);
  keys[1] = 0x8000000000000000ULL;
  OracleCase unit = sort_units::unit_of(1, keys);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(*oracle, unit)));
  unit.output = sort_units::sorted_output_of(keys);
  // Put the signed-order arrangement: the sign-bit key first.
  const std::size_t last = spec::kKeys - 1;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    std::swap(unit.output[spec::key_offset(0) + byte], unit.output[spec::key_offset(last) + byte]);
  }
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(unit.output[spec::index_offset(0) + byte],
              unit.output[spec::index_offset(last) + byte]);
  }
  EXPECT_TRUE(std::holds_alternative<ExactMismatch>(check(*oracle, unit)));
}

}  // namespace
}  // namespace loadforge::tests
