// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compression workload as a workload: the contract's suite against it,
// the factory's refusals at each boundary and in their order, the plan, the
// descriptor, the output layout spec.hpp fixes, and the scratch slot.

#include "workloads/compression/compression.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/compression/executors.hpp"
#include "unit/workloads/compression/support.hpp"
#include "verification/record.hpp"
#include "workloads/compression/generator.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Bytes;

constexpr std::uint64_t kUnitBytes = kInputBytes + output_bytes_for(kInputBytes);

WorkloadConfig config_of(std::uint64_t footprint, std::uint32_t decomposition = 0,
                         simd::IsaPath isa = simd::IsaPath::kScalar) {
  return WorkloadConfig{7, footprint, decomposition, isa};
}

std::unique_ptr<Workload> made(const WorkloadConfig& config) {
  auto created = create(config);
  EXPECT_TRUE(created.has_value());
  return created.has_value() ? std::move(created).value() : nullptr;
}

WorkloadError refused(const WorkloadConfig& config) {
  const auto created = create(config);
  EXPECT_FALSE(created.has_value());
  return created.has_value() ? WorkloadError{} : created.error();
}

}  // namespace
}  // namespace loadforge::workloads::compression

namespace loadforge::contracts {
namespace {

struct CompressionSubject {
  static constexpr const char* kName = "compression";
  static workloads::WorkloadConfig config() {
    return workloads::WorkloadConfig{42, 2 * workloads::compression::kUnitBytes, 0, {}};
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return workloads::compression::create(config);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Compression, WorkloadContract, CompressionSubject);

// The header also registers the registry's suite, which is the integration
// stream's to run: a workload is not a registry.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);

}  // namespace
}  // namespace loadforge::contracts

namespace loadforge::workloads::compression {
namespace {

TEST(CompressionWorkload, DescribesItselfAsTheBitExactExactScalarCompressionWorkload) {
  const auto workload = made(config_of(kUnitBytes));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->descriptor(),
            (Descriptor{"compression", kVersion, DeterminismClass::kBitExact,
                        verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}}));
  EXPECT_EQ(kName, "compression");
  EXPECT_EQ(kVersion, 1U);
}

TEST(CompressionWorkload, ThePlanIsTheSpecificationsGeometry) {
  const auto workload = made(config_of(10 * kUnitBytes + kUnitBytes - 1));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->plan(),
            (Plan{65536, 8 + 65536 + 257 + 16, 16384, 10, 1, 65536 + 8 + 65536 + 257 + 16, 65536}));
  EXPECT_EQ(workload->plan().scratch_bytes, kTableBytes);
}

TEST(CompressionWorkload, AFootprintOfExactlyOneUnitHoldsOneAndOneByteLessHoldsNone) {
  const auto one = made(config_of(kUnitBytes));
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(one->plan().units, 1U);
  for (const std::uint64_t footprint : {kUnitBytes - 1, std::uint64_t{0}, std::uint64_t{1}}) {
    const WorkloadError error = refused(config_of(footprint));
    EXPECT_EQ(error.fault, WorkloadFault::kFootprintTooSmall) << footprint;
    EXPECT_EQ(error.detail, std::to_string(footprint) + " bytes hold no unit; one takes " +
                                std::to_string(kUnitBytes));
  }
}

TEST(CompressionWorkload, TheLargestFootprintDividesWithoutOverflow) {
  const auto workload = made(config_of(std::numeric_limits<std::uint64_t>::max()));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->plan().units, std::numeric_limits<std::uint64_t>::max() / kUnitBytes);
}

TEST(CompressionWorkload, DecompositionZeroAndOneAreTheSameWorkloadAndNoOtherIsAccepted) {
  for (const std::uint32_t decomposition : {0U, 1U}) {
    const auto workload = made(config_of(kUnitBytes, decomposition));
    ASSERT_NE(workload, nullptr);
    EXPECT_EQ(workload->plan().decomposition, 1U) << decomposition;
  }
  for (const std::uint32_t decomposition :
       {2U, 3U, 7U, 64U, std::numeric_limits<std::uint32_t>::max()}) {
    const WorkloadError error = refused(config_of(kUnitBytes, decomposition));
    EXPECT_EQ(error.fault, WorkloadFault::kConfiguration);
    EXPECT_EQ(error.detail, "decomposition " + std::to_string(decomposition) +
                                ": compression has none, and accepts only 0 or 1");
  }
}

TEST(CompressionWorkload, EveryPathButScalarIsRefusedByName) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    const WorkloadError error = refused(config_of(kUnitBytes, 0, path));
    EXPECT_EQ(error.fault, WorkloadFault::kConfiguration);
    EXPECT_EQ(error.detail, "no kernel for the " + std::string(simd::describe(path)) +
                                " path; compression has the scalar kernel only");
  }
}

TEST(CompressionWorkload, RefusalsComeInTheirOrderPathThenDecompositionThenFootprint) {
  EXPECT_NE(refused(config_of(0, 9, simd::IsaPath::kNeon)).detail.find("no kernel"),
            std::string::npos);
  EXPECT_NE(refused(config_of(0, 9)).detail.find("decomposition 9"), std::string::npos);
}

TEST(CompressionWorkload, GenerateIsTheGeneratorUnderTheConfiguredSeed) {
  const auto workload = made(config_of(kUnitBytes));
  ASSERT_NE(workload, nullptr);
  Bytes expected(kInputBytes);
  generate_input(7, 12, expected);
  Bytes input(kInputBytes, std::byte{0x5A});
  workload->generate(12, input);
  EXPECT_EQ(input, expected);
}

TEST(CompressionWorkload, TheOutputIsTheMagicTheLengthTheBlockAndZeros) {
  Bytes input(kInputBytes);
  generate_input(3, 4, input);
  Bytes output(output_bytes_for(kInputBytes), std::byte{0xFF});
  Bytes table(kTableBytes, std::byte{0xFF});
  write_unit(input, output, table);

  const Bytes block = testing::compressed(input);
  EXPECT_EQ(load_le32(output, 0), kMagic);
  EXPECT_EQ(load_le32(output, 4), block.size());
  EXPECT_TRUE(
      std::ranges::equal(std::span<const std::byte>(output).subspan(8, block.size()), block));
  EXPECT_TRUE(std::ranges::all_of(std::span<const std::byte>(output).subspan(8 + block.size()),
                                  [](std::byte b) { return b == std::byte{0}; }))
      << "every byte after the block is zero, whatever was there";
  EXPECT_EQ(output[0], std::byte{'L'});
  EXPECT_EQ(output[3], std::byte{'1'});
}

TEST(CompressionWorkload, AnIncompressibleUnitFillsTheBoundWithNoPadding) {
  const Bytes input = testing::noise(kInputBytes, 11);
  Bytes output(output_bytes_for(kInputBytes), std::byte{0xFF});
  Bytes table(kTableBytes);
  write_unit(input, output, table);
  // One sequence of 65 536 literals: a token, 256 bytes of 255 and one of 241
  // (65 536 - 15 - 256 * 255), then the literals: the bound less 15.
  EXPECT_EQ(load_le32(output, 4), 1U + 257U + 65536U);
  EXPECT_EQ(output_bytes_for(kInputBytes) - 8 - load_le32(output, 4), 15U);
}

TEST(CompressionWorkload, ComputeUsesOnlyTheScratchOfTheSlotItRunsIn) {
  const auto workload = made(config_of(kUnitBytes));
  ASSERT_NE(workload, nullptr);
  Bytes input(kInputBytes);
  workload->generate(5, input);

  testing::SerialExecutor serial;
  Bytes expected(workload->plan().output_bytes);
  Bytes serial_scratch(workload->plan().scratch_bytes);
  workload->compute(5, input, expected, serial_scratch, serial);

  testing::LastSlotExecutor last(4);
  Bytes output(workload->plan().output_bytes);
  Bytes scratch(workload->plan().scratch_bytes * 4, std::byte{0xAB});
  workload->compute(5, input, output, scratch, last);
  EXPECT_EQ(output, expected);
  const std::span<const std::byte> others =
      std::span<const std::byte>(scratch).first(3 * workload->plan().scratch_bytes);
  EXPECT_TRUE(std::ranges::all_of(others, [](std::byte b) { return b == std::byte{0xAB}; }))
      << "slots 0 to 2 belong to other threads";
  EXPECT_FALSE(std::ranges::all_of(std::span<const std::byte>(scratch).subspan(others.size()),
                                   [](std::byte b) { return b == std::byte{0xAB}; }))
      << "slot 3 held the table";
}

TEST(CompressionWorkload, ComputeIsTheOutputOfWriteUnitForTheSameInput) {
  const auto workload = made(config_of(kUnitBytes));
  ASSERT_NE(workload, nullptr);
  const Bytes input = testing::noise(kInputBytes, 3);
  Bytes expected(workload->plan().output_bytes);
  Bytes table(kTableBytes);
  write_unit(input, expected, table);
  testing::SerialExecutor serial;
  Bytes output(workload->plan().output_bytes);
  Bytes scratch(workload->plan().scratch_bytes);
  workload->compute(0, input, output, scratch, serial);
  EXPECT_EQ(output, expected) << "the unit's index is not an input to compute";
}

}  // namespace
}  // namespace loadforge::workloads::compression
