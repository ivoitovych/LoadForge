// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_CONTRACT_HPP

// Provider-side suites for src/verification/oracle.hpp (ADR 0003, 0004).
//
// OracleContract<Subject>:
//   Subject::kWorkload   the workload's name.
//   Subject::kContract   verification::Contract::kExact or kBounded.
//   Subject::make()      std::unique_ptr<Oracle>.
//   Subject::good()      std::vector<OracleCase>: units whose output is right
//                        by the specification -- from the reference tool or a
//                        fixture where the specification allows, never from
//                        the stress implementation (verification.md §2).
//   Subject::wrong()     std::vector<OracleCase>: units whose output the
//                        specification says is wrong, each of which the
//                        oracle must catch. Each must be caught whatever the
//                        oracle samples -- choose corruptions in every block,
//                        or in the blocks the index samples.
//
// OracleRegistryContract<Subject>:
//   Subject::make()        std::unique_ptr<OracleRegistry>.
//   Subject::config(name)  a WorkloadConfig, and Subject::plan(name) the Plan,
//                          for which the named oracle can be created.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "verification/oracle.hpp"

namespace loadforge::contracts {

struct OracleCase {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

namespace oracle_suite {

inline verification::Verdict check(const verification::Oracle& oracle, const OracleCase& unit) {
  return oracle.check(verification::UnitEvidence{unit.index, unit.input, unit.output});
}

inline std::string name_of(const verification::Verdict& verdict) {
  switch (verdict.index()) {
    case 0:
      return "Agreement";
    case 1:
      return "ExactMismatch";
    case 2:
      return "BoundedMismatch";
    default:
      return "NotChecked";
  }
}

}  // namespace oracle_suite

template <typename Subject>
class OracleContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(OracleContract);

TYPED_TEST_P(OracleContract, ItNamesItsWorkloadAndContract) {
  const auto oracle = TypeParam::make();
  EXPECT_EQ(oracle->workload(), TypeParam::kWorkload);
  EXPECT_EQ(oracle->contract(), TypeParam::kContract);
}

TYPED_TEST_P(OracleContract, ARightOutputIsAgreedWithAfterComparingSomething) {
  const auto oracle = TypeParam::make();
  const std::vector<OracleCase> cases = TypeParam::good();
  ASSERT_FALSE(cases.empty());
  for (const OracleCase& unit : cases) {
    const verification::Verdict verdict = oracle_suite::check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<verification::Agreement>(verdict))
        << "unit " << unit.index << ": " << oracle_suite::name_of(verdict);
    EXPECT_GE(verification::blocks_of(verdict), 1U);
    EXPECT_TRUE(verification::is_valid(verdict, TypeParam::kContract));
  }
}

TYPED_TEST_P(OracleContract, AWrongOutputIsCaughtAsTheContractsKindOfMismatch) {
  const auto oracle = TypeParam::make();
  const std::vector<OracleCase> cases = TypeParam::wrong();
  ASSERT_FALSE(cases.empty()) << "an oracle never shown a wrong output is unproven (§6)";
  for (const OracleCase& unit : cases) {
    const verification::Verdict verdict = oracle_suite::check(*oracle, unit);
    if (TypeParam::kContract == verification::Contract::kExact) {
      EXPECT_TRUE(std::holds_alternative<verification::ExactMismatch>(verdict))
          << "unit " << unit.index << ": " << oracle_suite::name_of(verdict);
    } else {
      EXPECT_TRUE(std::holds_alternative<verification::BoundedMismatch>(verdict))
          << "unit " << unit.index << ": " << oracle_suite::name_of(verdict);
    }
    EXPECT_TRUE(verification::is_valid(verdict, TypeParam::kContract)) << "unit " << unit.index;
  }
}

TYPED_TEST_P(OracleContract, EvidenceOfTheWrongSizeIsNotCheckedNeverAgreed) {
  const auto oracle = TypeParam::make();
  const OracleCase good = TypeParam::good().front();
  std::vector<OracleCase> malformed(5, good);
  malformed[0].output.pop_back();
  malformed[1].output.push_back(std::byte{0});
  malformed[2].output.clear();
  malformed[3].input.pop_back();
  malformed[4].input.clear();
  for (std::size_t which = 0; which < malformed.size(); ++which) {
    const verification::Verdict verdict = oracle_suite::check(*oracle, malformed[which]);
    ASSERT_TRUE(std::holds_alternative<verification::NotChecked>(verdict))
        << "case " << which << ": " << oracle_suite::name_of(verdict);
    EXPECT_TRUE(verification::is_valid(verdict, TypeParam::kContract)) << "case " << which;
  }
}

TYPED_TEST_P(OracleContract, TheVerdictIsAFunctionOfTheEvidence) {
  const auto oracle = TypeParam::make();
  std::vector<OracleCase> cases = TypeParam::good();
  const std::vector<OracleCase> wrong = TypeParam::wrong();
  cases.insert(cases.end(), wrong.begin(), wrong.end());
  for (const OracleCase& unit : cases) {
    const verification::Verdict first = oracle_suite::check(*oracle, unit);
    const verification::Verdict second = oracle_suite::check(*TypeParam::make(), unit);
    EXPECT_EQ(first.index(), second.index()) << "unit " << unit.index;
    EXPECT_EQ(verification::blocks_of(first), verification::blocks_of(second));
  }
}

TYPED_TEST_P(OracleContract, ChecksRunConcurrently) {
  const auto oracle = TypeParam::make();
  std::vector<OracleCase> cases = TypeParam::good();
  const std::vector<OracleCase> wrong = TypeParam::wrong();
  cases.insert(cases.end(), wrong.begin(), wrong.end());
  std::vector<std::size_t> alone;
  for (const OracleCase& unit : cases) {
    alone.push_back(oracle_suite::check(*oracle, unit).index());
  }
  std::vector<std::size_t> together(cases.size());
  std::vector<std::thread> threads;
  for (std::size_t which = 0; which < cases.size(); ++which) {
    threads.emplace_back(
        [&, which] { together[which] = oracle_suite::check(*oracle, cases[which]).index(); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(together, alone);
}

REGISTER_TYPED_TEST_SUITE_P(OracleContract, ItNamesItsWorkloadAndContract,
                            ARightOutputIsAgreedWithAfterComparingSomething,
                            AWrongOutputIsCaughtAsTheContractsKindOfMismatch,
                            EvidenceOfTheWrongSizeIsNotCheckedNeverAgreed,
                            TheVerdictIsAFunctionOfTheEvidence, ChecksRunConcurrently);

template <typename Subject>
class OracleRegistryContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(OracleRegistryContract);

TYPED_TEST_P(OracleRegistryContract, EachNameCreatesTheOracleForThatWorkload) {
  const auto registry = TypeParam::make();
  const std::vector<std::string> names = registry->names();
  EXPECT_FALSE(names.empty());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
  for (const std::string& name : names) {
    const auto created = registry->create(name, TypeParam::config(name), TypeParam::plan(name));
    ASSERT_TRUE(created.has_value()) << name << ": " << created.error().detail;
    EXPECT_EQ(created.value()->workload(), name);
  }
}

TYPED_TEST_P(OracleRegistryContract, AnUnknownNameIsRefusedAsUnknown) {
  const auto registry = TypeParam::make();
  const auto created = registry->create("no_such_workload", workloads::WorkloadConfig{},
                                        workloads::Plan{1, 1, 0, 1, 1, 0, 0});
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, verification::OracleFault::kUnknownWorkload);
}

REGISTER_TYPED_TEST_SUITE_P(OracleRegistryContract, EachNameCreatesTheOracleForThatWorkload,
                            AnUnknownNameIsRefusedAsUnknown);

}  // namespace loadforge::contracts

#endif
