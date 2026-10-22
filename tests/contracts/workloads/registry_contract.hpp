// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_WORKLOADS_REGISTRY_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_WORKLOADS_REGISTRY_CONTRACT_HPP

// Provider-side suite for the registry in src/workloads/workload.hpp
// (ADR 0003, 0004).
//
// RegistryContract<Subject>:
//   Subject::make()     std::unique_ptr<Registry>.
//   Subject::config(n)  a WorkloadConfig workload n accepts.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {

template <typename Subject>
class RegistryContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(RegistryContract);

TYPED_TEST_P(RegistryContract, NamesAreSortedUniqueAndValid) {
  const auto registry = TypeParam::make();
  const std::vector<std::string> names = registry->names();
  EXPECT_FALSE(names.empty());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
  for (const std::string& name : names) {
    EXPECT_TRUE(workloads::is_valid_name(name)) << name;
  }
}

TYPED_TEST_P(RegistryContract, EachNameCreatesTheWorkloadOfThatName) {
  const auto registry = TypeParam::make();
  for (const std::string& name : registry->names()) {
    const auto created = registry->create(name, TypeParam::config(name));
    ASSERT_TRUE(created.has_value()) << name << ": " << created.error().detail;
    EXPECT_EQ(created.value()->descriptor().name, name);
  }
}

TYPED_TEST_P(RegistryContract, AnUnknownNameIsRefusedAsUnknown) {
  const auto registry = TypeParam::make();
  for (const std::string_view name : {"no_such_workload", "", "Compression"}) {
    const auto created = registry->create(name, workloads::WorkloadConfig{1, 1U << 20U, 0, {}});
    ASSERT_FALSE(created.has_value()) << name;
    EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kUnknownWorkload);
  }
}

// Contract change of the M2 corrections (2026-10-21): descriptor(name) answers
// what create() would, without creating.
TYPED_TEST_P(RegistryContract, ADescriptorIsWhatTheCreatedWorkloadSaysAboutItself) {
  const auto registry = TypeParam::make();
  for (const std::string& name : registry->names()) {
    const auto described = registry->descriptor(name);
    ASSERT_TRUE(described.has_value()) << name << ": " << described.error().detail;
    EXPECT_EQ(described.value().name, name);
    EXPECT_TRUE(described.value().paths.contains(simd::IsaPath::kScalar)) << name;
    const auto created = registry->create(name, TypeParam::config(name));
    ASSERT_TRUE(created.has_value()) << name << ": " << created.error().detail;
    EXPECT_EQ(described.value(), created.value()->descriptor()) << name;
    EXPECT_EQ(registry->descriptor(name).value(), described.value())
        << name << ": the same on every call";
  }
}

TYPED_TEST_P(RegistryContract, AnUnknownNameHasNoDescriptorAndIsRefusedAsCreateRefusesIt) {
  const auto registry = TypeParam::make();
  for (const std::string_view name : {"no_such_workload", "", "Compression"}) {
    const auto described = registry->descriptor(name);
    ASSERT_FALSE(described.has_value()) << name;
    EXPECT_EQ(described.error().fault, workloads::WorkloadFault::kUnknownWorkload);
    const auto created = registry->create(name, TypeParam::config(std::string{name}));
    ASSERT_FALSE(created.has_value()) << name;
    EXPECT_EQ(described.error(), created.error()) << "the same words as create()";
  }
}

REGISTER_TYPED_TEST_SUITE_P(RegistryContract, NamesAreSortedUniqueAndValid,
                            EachNameCreatesTheWorkloadOfThatName, AnUnknownNameIsRefusedAsUnknown,
                            ADescriptorIsWhatTheCreatedWorkloadSaysAboutItself,
                            AnUnknownNameHasNoDescriptorAndIsRefusedAsCreateRefusesIt);

}  // namespace loadforge::contracts

#endif
