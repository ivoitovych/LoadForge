// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the workload registry: the suite of the contract it provides, and
// the facts about this build's table that no contract can state.

#include "workloads/registry/registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "contracts/workloads/registry_contract.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {
namespace {

using workloads::BuiltinRegistry;
using workloads::WorkloadConfig;
using workloads::WorkloadFault;

constexpr std::uint64_t kFootprint = std::uint64_t{16} << 20U;

struct BuiltinRegistrySubject {
  static std::unique_ptr<workloads::Registry> make() { return std::make_unique<BuiltinRegistry>(); }
  static WorkloadConfig config(const std::string& /*name*/) {
    return WorkloadConfig{7, kFootprint, 0, simd::IsaPath::kScalar};
  }
};

using Subjects = ::testing::Types<BuiltinRegistrySubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(BuiltinRegistry, RegistryContract, Subjects);

TEST(BuiltinRegistryTable, ListsTheWorkloadsThisBuildContains) {
  const BuiltinRegistry registry;
  EXPECT_EQ(registry.names(),
            (std::vector<std::string>{"compression", "dense_linear", "monte_carlo", "sort"}));
}

TEST(BuiltinRegistryTable, EachWorkloadIsCreatedByItsOwnFactoryAndKeepsItsContract) {
  const BuiltinRegistry registry;
  for (const std::string& name : registry.names()) {
    const auto created = registry.create(name, BuiltinRegistrySubject::config(name));
    ASSERT_TRUE(created.has_value()) << name << ": " << created.error().detail;
    const workloads::Descriptor& descriptor = created.value()->descriptor();
    EXPECT_EQ(descriptor.name, name);
    EXPECT_GE(descriptor.version, 1U);
    EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar)) << name;
    EXPECT_GE(created.value()->plan().units, 1U) << name;
  }
}

TEST(BuiltinRegistryTable, ACompressionWorkloadIsExactAndDenseLinearIsBounded) {
  const BuiltinRegistry registry;
  const auto compression =
      registry.create("compression", BuiltinRegistrySubject::config("compression"));
  ASSERT_TRUE(compression.has_value());
  EXPECT_EQ(compression.value()->descriptor().contract, verification::Contract::kExact);
  const auto dense =
      registry.create("dense_linear", BuiltinRegistrySubject::config("dense_linear"));
  ASSERT_TRUE(dense.has_value());
  EXPECT_EQ(dense.value()->descriptor().contract, verification::Contract::kBounded);
}

TEST(BuiltinRegistryTable, AWorkloadsOwnRefusalIsPassedOnUnchanged) {
  const BuiltinRegistry registry;
  const auto too_small = registry.create("compression", WorkloadConfig{1, 1, 0, {}});
  ASSERT_FALSE(too_small.has_value());
  EXPECT_EQ(too_small.error().fault, WorkloadFault::kFootprintTooSmall);
  const auto odd_panel = registry.create(
      "dense_linear", WorkloadConfig{1, kFootprint, 999983, simd::IsaPath::kScalar});
  ASSERT_FALSE(odd_panel.has_value());
  EXPECT_EQ(odd_panel.error().fault, WorkloadFault::kConfiguration);
}

TEST(BuiltinRegistryTable, AnUnknownNameIsNamedInTheRefusal) {
  const BuiltinRegistry registry;
  const auto created = registry.create("fft", BuiltinRegistrySubject::config("fft"));
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, WorkloadFault::kUnknownWorkload);
  EXPECT_NE(created.error().detail.find("'fft'"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::contracts
