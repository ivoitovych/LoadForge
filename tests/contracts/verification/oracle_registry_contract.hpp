// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_REGISTRY_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_REGISTRY_CONTRACT_HPP

// Provider-side suite for the oracle registry in src/verification/oracle.hpp
// (ADR 0003, 0004).
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
