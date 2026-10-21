// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the oracle registry: the suite of the contract it provides, and the
// two facts about this build that tie it to the workload registry.

#include "verification/oracle/registry/registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "contracts/verification/oracle_registry_contract.hpp"
#include "workloads/registry/registry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {
namespace {

using verification::BuiltinOracleRegistry;
using verification::OracleFault;
using workloads::WorkloadConfig;

constexpr std::uint64_t kFootprint = std::uint64_t{16} << 20U;

WorkloadConfig config_of(const std::string& /*name*/) {
  return WorkloadConfig{7, kFootprint, 0, simd::IsaPath::kScalar};
}

workloads::Plan plan_of(const std::string& name) {
  const workloads::BuiltinRegistry workloads;
  const auto created = workloads.create(name, config_of(name));
  EXPECT_TRUE(created.has_value()) << name;
  return created.value()->plan();
}

struct BuiltinOracleRegistrySubject {
  static std::unique_ptr<verification::OracleRegistry> make() {
    return std::make_unique<BuiltinOracleRegistry>();
  }
  static WorkloadConfig config(const std::string& name) { return config_of(name); }
  static workloads::Plan plan(const std::string& name) { return plan_of(name); }
};

using Subjects = ::testing::Types<BuiltinOracleRegistrySubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(BuiltinOracleRegistry, OracleRegistryContract, Subjects);

TEST(BuiltinOracleRegistryTable, NamesExactlyTheWorkloadsOfTheWorkloadRegistry) {
  const BuiltinOracleRegistry oracles;
  const workloads::BuiltinRegistry workloads;
  EXPECT_EQ(oracles.names(), workloads.names());
}

TEST(BuiltinOracleRegistryTable, EachOracleHoldsTheContractItsWorkloadDeclares) {
  const BuiltinOracleRegistry oracles;
  const workloads::BuiltinRegistry workloads;
  for (const std::string& name : workloads.names()) {
    const auto workload = workloads.create(name, config_of(name));
    ASSERT_TRUE(workload.has_value()) << name;
    const auto oracle = oracles.create(name, config_of(name), workload.value()->plan());
    ASSERT_TRUE(oracle.has_value()) << name << ": " << oracle.error().detail;
    EXPECT_EQ(oracle.value()->contract(), workload.value()->descriptor().contract) << name;
  }
}

TEST(BuiltinOracleRegistryTable, AnOraclesOwnRefusalIsPassedOnUnchanged) {
  const BuiltinOracleRegistry oracles;
  const auto refused =
      oracles.create("compression", config_of("compression"), workloads::Plan{1, 1, 0, 1, 1, 0, 0});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, OracleFault::kConfiguration);
}

TEST(BuiltinOracleRegistryTable, AnUnknownNameIsNamedInTheRefusal) {
  const BuiltinOracleRegistry oracles;
  const auto created = oracles.create("fft", WorkloadConfig{}, workloads::Plan{});
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, OracleFault::kUnknownWorkload);
  EXPECT_NE(created.error().detail.find("'fft'"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::contracts
