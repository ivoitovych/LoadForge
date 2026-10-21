// SPDX-License-Identifier: GPL-3.0-or-later
// The Monte Carlo workload: WorkloadContract against the implementation, and
// what the contract leaves to each workload -- the configuration's domain, the
// plan's numbers, the bytes of an input and of an output.

#include "workloads/workload.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "contracts/workloads/workload_contract.hpp"
#include "core/result.hpp"
#include "oracle/workloads/monte_carlo/replay.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "verification/record.hpp"
#include "workloads/monte_carlo/monte_carlo.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo {

namespace {

using testing::compute_unit;
using testing::generate_unit;
using testing::make;
using testing::native_path;
using testing::SerialExecutor;

struct ScalarSubject {
  static constexpr const char* kName = "monte_carlo";
  static WorkloadConfig config() { return WorkloadConfig{0x5EED, 8192, 0, simd::IsaPath::kScalar}; }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return monte_carlo::create(c);
  }
};

/// The machine's vector path, and a chunk length that is not the default.
struct NativeSubject : ScalarSubject {
  static WorkloadConfig config() { return WorkloadConfig{99, 4096, 256, native_path()}; }
};

constexpr std::uint64_t kUnitBytes = kInputBytes + kOutputBytes;

WorkloadConfig scalar_config() { return ScalarSubject::config(); }

TEST(MonteCarloDescriptor, SaysWhatTheSpecificationSays) {
  const auto workload = make(1, 0, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "monte_carlo");
  EXPECT_EQ(descriptor.name, kName);
  EXPECT_EQ(descriptor.version, kVersion);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx512));
  EXPECT_TRUE(descriptor.paths.contains(native_path()));
}

TEST(MonteCarloPlan, CountsOneUnitsBytesAndSamples) {
  const auto workload = make(1, 0, simd::IsaPath::kScalar, 5000);
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, kInputBytes);
  EXPECT_EQ(plan.output_bytes, kOutputBytes);
  EXPECT_EQ(plan.scratch_bytes, 0U);
  EXPECT_EQ(plan.units, 4U);
  EXPECT_EQ(plan.logical_bytes, kUnitBytes);
  EXPECT_EQ(plan.operations, kBatches * kSamplesPerBatch);
  EXPECT_EQ(&workload->plan(), &plan) << "constant for the workload's lifetime";
}

TEST(MonteCarloPlan, TheFootprintDecidesTheUnitsAtItsBoundaries) {
  struct Case {
    std::uint64_t footprint;
    std::uint64_t units;
  };
  for (const Case& c : {Case{kUnitBytes, 1}, Case{2 * kUnitBytes - 1, 1}, Case{2 * kUnitBytes, 2},
                        Case{~std::uint64_t{0}, ~std::uint64_t{0} / kUnitBytes}}) {
    const auto workload = make(1, 0, simd::IsaPath::kScalar, c.footprint);
    ASSERT_NE(workload, nullptr) << c.footprint;
    EXPECT_EQ(workload->plan().units, c.units) << c.footprint;
  }
}

TEST(MonteCarloPlan, ResolvesTheDecomposition) {
  const auto by_default = make(1, 0, simd::IsaPath::kScalar);
  ASSERT_NE(by_default, nullptr);
  EXPECT_EQ(by_default->plan().decomposition, kDefaultChunkLength);
  for (const std::uint32_t chunk : kChunkLengths) {
    const auto workload = make(1, chunk, simd::IsaPath::kScalar);
    ASSERT_NE(workload, nullptr) << chunk;
    EXPECT_EQ(workload->plan().decomposition, chunk);
  }
}

TEST(MonteCarloRefusals, ADecompositionOutsideTheChunkLengthsIsAConfigurationFault) {
  for (const std::uint32_t chunk :
       {1U, 63U, 65U, 128U, 1025U, 16383U, 16385U, 32768U, ~std::uint32_t{0}}) {
    WorkloadConfig config = scalar_config();
    config.decomposition = chunk;
    const auto created = create(config);
    ASSERT_FALSE(created.has_value()) << chunk;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(chunk)), std::string::npos)
        << created.error().detail;
  }
}

TEST(MonteCarloRefusals, APathWithoutAKernelIsRefusedByName) {
  const Descriptor descriptor = make(1, 0, simd::IsaPath::kScalar)->descriptor();
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (descriptor.paths.contains(path)) {
      continue;
    }
    WorkloadConfig config = scalar_config();
    config.isa = path;
    const auto created = create(config);
    ASSERT_FALSE(created.has_value()) << simd::describe(path);
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::string(simd::describe(path))), std::string::npos)
        << created.error().detail;
  }
}

TEST(MonteCarloRefusals, AFootprintUnderOneUnitIsTooSmallAtTheBoundary) {
  for (const std::uint64_t footprint : {std::uint64_t{0}, std::uint64_t{1}, kUnitBytes - 1}) {
    WorkloadConfig config = scalar_config();
    config.footprint_bytes = footprint;
    const auto created = create(config);
    ASSERT_FALSE(created.has_value()) << footprint;
    EXPECT_EQ(created.error().fault, WorkloadFault::kFootprintTooSmall);
    EXPECT_NE(created.error().detail.find(std::to_string(footprint)), std::string::npos);
  }
}

TEST(MonteCarloRefusals, ThePathIsJudgedBeforeTheDecompositionAndThatBeforeTheFootprint) {
  WorkloadConfig config{1, 1, 3, simd::IsaPath::kAvx512};
  EXPECT_NE(create(config).error().detail.find("avx512"), std::string::npos);
  config.isa = simd::IsaPath::kScalar;
  EXPECT_EQ(create(config).error().fault, WorkloadFault::kConfiguration);
  config.decomposition = 64;
  EXPECT_EQ(create(config).error().fault, WorkloadFault::kFootprintTooSmall);
}

TEST(MonteCarloGenerate, WritesBatchKeysLittleEndian) {
  const auto workload = make(0xFEEDFACE, 0, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{9}, ~std::uint64_t{0}}) {
    const std::vector<std::byte> input = generate_unit(*workload, index);
    ASSERT_EQ(input.size(), kInputBytes);
    for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
      const std::uint64_t key = batch_key(0xFEEDFACE, index, batch);
      for (std::size_t byte = 0; byte < 8; ++byte) {
        EXPECT_EQ(static_cast<std::uint8_t>(input[batch * 8 + byte]),
                  static_cast<std::uint8_t>(key >> (8 * byte)))
            << "index " << index << " batch " << batch << " byte " << byte;
      }
    }
  }
}

TEST(MonteCarloGenerate, GivesDistinctBatchesDistinctKeys) {
  const auto workload = make(3, 0, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  const std::vector<std::byte> input = generate_unit(*workload, 4);
  for (std::uint64_t a = 0; a < kBatches; ++a) {
    for (std::uint64_t b = a + 1; b < kBatches; ++b) {
      EXPECT_NE(replay::key_of(input, a), replay::key_of(input, b)) << a << " " << b;
    }
  }
}

TEST(MonteCarloCompute, WritesEachBatchSumAsALittleEndianDoubleInItsSlot) {
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    const auto workload = make(21, 64, path);
    ASSERT_NE(workload, nullptr);
    const std::vector<std::byte> input = generate_unit(*workload, 2);
    const std::vector<std::byte> output = compute_unit(*workload, 2, input, serial);
    ASSERT_EQ(output.size(), kOutputBytes);
    for (const std::uint64_t batch :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{31}, kBatches - 1}) {
      const double expected =
          replay::ordered_sum(replay::key_of(input, batch), testing::lanes_of(path), 64);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(
                    replay::load_double(std::span<const std::byte>(output).subspan(batch * 8, 8))),
                std::bit_cast<std::uint64_t>(expected))
          << simd::describe(path) << " batch " << batch;
    }
  }
}

TEST(MonteCarloCompute, ReadsTheKeyFromTheInputNotFromTheIndex) {
  SerialExecutor serial;
  const auto workload = make(21, 0, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  const std::vector<std::byte> input = generate_unit(*workload, 2);
  EXPECT_EQ(compute_unit(*workload, 2, input, serial), compute_unit(*workload, 777, input, serial))
      << "the output is a function of the input";
  std::vector<std::byte> other = input;
  other[0] ^= std::byte{1};
  EXPECT_NE(compute_unit(*workload, 2, other, serial), compute_unit(*workload, 2, input, serial));
}

TEST(MonteCarloCompute, CoversTheEdgesOfTheKeyDomain) {
  SerialExecutor serial;
  const auto workload = make(1, 4096, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  for (const std::byte fill : {std::byte{0x00}, std::byte{0xFF}}) {
    const std::vector<std::byte> input(kInputBytes, fill);
    const std::vector<std::byte> output = compute_unit(*workload, 0, input, serial);
    const std::uint64_t key = fill == std::byte{0} ? 0 : ~std::uint64_t{0};
    const double expected = replay::ordered_sum(key, 1, 4096);
    EXPECT_EQ(replay::load_double(std::span<const std::byte>(output).subspan(0, 8)), expected);
    EXPECT_GE(expected, static_cast<double>(kSamplesPerBatch) * 0x1p-26);
    EXPECT_LE(expected, static_cast<double>(kSamplesPerBatch));
    for (std::size_t batch = 1; batch < kBatches; ++batch) {
      EXPECT_EQ(std::vector<std::byte>(output.begin() + 8 * static_cast<long>(batch),
                                       output.begin() + 8 * static_cast<long>(batch) + 8),
                std::vector<std::byte>(output.begin(), output.begin() + 8))
          << "equal keys give equal sums, in every slot";
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo

// The suites' macros name symbols in the contracts' namespace. The Registry
// suite is registered by the same header; the registry is the integration
// stream's, not this module's.
namespace loadforge::contracts {

using MonteCarloSubjects =
    ::testing::Types<workloads::monte_carlo::ScalarSubject, workloads::monte_carlo::NativeSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(MonteCarlo, WorkloadContract, MonteCarloSubjects);

}  // namespace loadforge::contracts
