// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_WORKLOADS_WORKLOAD_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_WORKLOADS_WORKLOAD_CONTRACT_HPP

// Provider-side suites for src/workloads/workload.hpp (ADR 0003, 0004).
//
// WorkloadContract<Subject>:
//   Subject::kName      the workload's name.
//   Subject::config()   a WorkloadConfig the workload accepts, whose
//                       footprint holds at least two units.
//   Subject::create(c)  core::Result<std::unique_ptr<Workload>, WorkloadError>.
//
// RegistryContract<Subject>:
//   Subject::make()     std::unique_ptr<Registry>.
//   Subject::config(n)  a WorkloadConfig workload n accepts.
//
// The suite brings its own executors -- one serial, one with three threads --
// because "the thread count does not change the output" is a claim about
// executors the workload did not choose.

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

namespace workload_suite {

class Serial final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

class ThreeThreads final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 3; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < 3; ++slot) {
      // Reverse order within each slot, so the order of calls differs from
      // the serial executor's as well as their threads.
      pool.emplace_back([slot, count, task, context] {
        for (std::size_t index = count; index-- > 0;) {
          if (index % 3 == slot) {
            task(context, index, slot);
          }
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }
};

using Bytes = std::vector<std::byte>;

inline Bytes generated(const workloads::Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

inline Bytes computed(const workloads::Workload& workload, std::uint64_t index, const Bytes& input,
                      worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

}  // namespace workload_suite

template <typename Subject>
class WorkloadContract : public ::testing::Test {
 protected:
  static std::unique_ptr<workloads::Workload> make(const workloads::WorkloadConfig& config) {
    auto created = Subject::create(config);
    EXPECT_TRUE(created.has_value()) << created.error().detail;
    return created.has_value() ? std::move(created).value() : nullptr;
  }
};
TYPED_TEST_SUITE_P(WorkloadContract);

using workload_suite::Bytes;

TYPED_TEST_P(WorkloadContract, TheDescriptorKeepsItsRules) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  const workloads::Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, TypeParam::kName);
  EXPECT_TRUE(workloads::is_valid_name(descriptor.name));
  EXPECT_GE(descriptor.version, 1U);
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar));
  if (descriptor.determinism == workloads::DeterminismClass::kBitExact) {
    EXPECT_EQ(descriptor.contract, verification::Contract::kExact);
  }
  if (descriptor.determinism == workloads::DeterminismClass::kBoundedResidual) {
    EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  }
  EXPECT_EQ(&workload->descriptor(), &descriptor) << "constant for the workload's lifetime";
}

TYPED_TEST_P(WorkloadContract, ThePlanFitsTheFootprint) {
  const workloads::WorkloadConfig config = TypeParam::config();
  const auto workload = this->make(config);
  ASSERT_NE(workload, nullptr);
  const workloads::Plan& plan = workload->plan();
  EXPECT_GE(plan.input_bytes, 1U);
  EXPECT_GE(plan.output_bytes, 1U);
  EXPECT_GE(plan.units, 2U) << "the subject's config holds two units, by the suite's rules";
  EXPECT_LE(plan.units * (plan.input_bytes + plan.output_bytes), config.footprint_bytes);
  EXPECT_NE(plan.decomposition, 0U);
  if (config.decomposition != 0) {
    EXPECT_EQ(plan.decomposition, config.decomposition);
  }
}

TYPED_TEST_P(WorkloadContract, AFootprintTooSmallForOneUnitIsRefusedAsSuch) {
  workloads::WorkloadConfig config = TypeParam::config();
  config.footprint_bytes = 1;
  const auto created = TypeParam::create(config);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kFootprintTooSmall);
  EXPECT_FALSE(created.error().detail.empty());
}

TYPED_TEST_P(WorkloadContract, AnIsaPathWithoutAKernelIsRefusedNotReplaced) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (workload->descriptor().paths.contains(path)) {
      continue;
    }
    workloads::WorkloadConfig config = TypeParam::config();
    config.isa = path;
    const auto created = TypeParam::create(config);
    ASSERT_FALSE(created.has_value()) << simd::describe(path);
    EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  }
}

TYPED_TEST_P(WorkloadContract, InputIsAPureFunctionOfSeedAndIndex) {
  const workloads::WorkloadConfig config = TypeParam::config();
  const auto workload = this->make(config);
  ASSERT_NE(workload, nullptr);
  const Bytes first = workload_suite::generated(*workload, 1);
  EXPECT_EQ(workload_suite::generated(*workload, 0), workload_suite::generated(*workload, 0));
  EXPECT_EQ(workload_suite::generated(*workload, 1), first) << "after another index: no state";
  EXPECT_NE(workload_suite::generated(*workload, 0), first) << "distinct units, distinct inputs";
  EXPECT_EQ(workload_suite::generated(*workload, ~std::uint64_t{0}),
            workload_suite::generated(*workload, ~std::uint64_t{0}))
      << "every index is a unit";

  workloads::WorkloadConfig scalar = config;
  scalar.isa = simd::IsaPath::kScalar;
  const auto other = this->make(scalar);
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(workload_suite::generated(*other, 1), first) << "the ISA path does not touch input";

  workloads::WorkloadConfig reseeded = config;
  reseeded.seed = config.seed + 1;
  const auto third = this->make(reseeded);
  ASSERT_NE(third, nullptr);
  EXPECT_NE(workload_suite::generated(*third, 1), first) << "the seed selects the data";
}

TYPED_TEST_P(WorkloadContract, ComputeWritesEveryOutputByteWhateverWasThere) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  workload_suite::Serial serial;
  const Bytes input = workload_suite::generated(*workload, 3);
  EXPECT_EQ(workload_suite::computed(*workload, 3, input, serial, std::byte{0x00}),
            workload_suite::computed(*workload, 3, input, serial, std::byte{0xFF}))
      << "output and scratch prefilled with zeros, then with ones";
}

TYPED_TEST_P(WorkloadContract, TheThreadCountNeverChangesAReproducibleOutput) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  if (workload->descriptor().determinism == workloads::DeterminismClass::kBoundedResidual) {
    GTEST_SKIP() << "BoundedResidual promises a tolerance, which the workload's T6 tests own";
  }
  workload_suite::Serial serial;
  workload_suite::ThreeThreads three;
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{5}}) {
    const Bytes input = workload_suite::generated(*workload, index);
    EXPECT_EQ(workload_suite::computed(*workload, index, input, serial),
              workload_suite::computed(*workload, index, input, three))
        << "unit " << index;
  }
}

TYPED_TEST_P(WorkloadContract, ComputeIsTotalOverCorruptInput) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  workload_suite::Serial serial;
  const std::size_t size = workload->plan().input_bytes;
  std::vector<Bytes> inputs = {Bytes(size, std::byte{0x00}), Bytes(size, std::byte{0xFF}),
                               Bytes(size, std::byte{0x7F})};
  Bytes noise(size);
  rng::fill_stream(0xBAD, 0, noise);
  inputs.push_back(noise);
  Bytes flipped = workload_suite::generated(*workload, 2);
  for (std::size_t at = 0; at < flipped.size(); at += 7) {
    flipped[at] ^= std::byte{0x80};
  }
  inputs.push_back(flipped);
  for (const Bytes& input : inputs) {
    const Bytes once = workload_suite::computed(*workload, 2, input, serial);
    EXPECT_EQ(once.size(), workload->plan().output_bytes);
    if (workload->descriptor().determinism != workloads::DeterminismClass::kBoundedResidual) {
      EXPECT_EQ(workload_suite::computed(*workload, 2, input, serial), once)
          << "still a function of its input";
    }
  }
}

TYPED_TEST_P(WorkloadContract, DistinctUnitsComputeConcurrently) {
  const auto workload = this->make(TypeParam::config());
  ASSERT_NE(workload, nullptr);
  workload_suite::Serial serial;
  std::vector<Bytes> inputs;
  std::vector<Bytes> alone;
  for (std::uint64_t index = 0; index < 4; ++index) {
    inputs.push_back(workload_suite::generated(*workload, index));
    alone.push_back(workload_suite::computed(*workload, index, inputs.back(), serial));
  }
  std::vector<Bytes> together(4);
  std::vector<std::thread> threads;
  for (std::uint64_t index = 0; index < 4; ++index) {
    threads.emplace_back([&, index] {
      workload_suite::Serial own;
      together[index] = workload_suite::computed(*workload, index, inputs[index], own);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  if (workload->descriptor().determinism != workloads::DeterminismClass::kBoundedResidual) {
    EXPECT_EQ(together, alone);
  }
}

REGISTER_TYPED_TEST_SUITE_P(WorkloadContract, TheDescriptorKeepsItsRules, ThePlanFitsTheFootprint,
                            AFootprintTooSmallForOneUnitIsRefusedAsSuch,
                            AnIsaPathWithoutAKernelIsRefusedNotReplaced,
                            InputIsAPureFunctionOfSeedAndIndex,
                            ComputeWritesEveryOutputByteWhateverWasThere,
                            TheThreadCountNeverChangesAReproducibleOutput,
                            ComputeIsTotalOverCorruptInput, DistinctUnitsComputeConcurrently);

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

REGISTER_TYPED_TEST_SUITE_P(RegistryContract, NamesAreSortedUniqueAndValid,
                            EachNameCreatesTheWorkloadOfThatName, AnUnknownNameIsRefusedAsUnknown);

}  // namespace loadforge::contracts

#endif
