// SPDX-License-Identifier: GPL-3.0-or-later
//
// What create() accepts and refuses, and the plan, descriptor and sizes it
// makes. The numbers are written out, not computed from the constants they
// check, so a changed constant shows.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "workloads/sparse/sparse.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sparse {
namespace {

constexpr std::uint64_t kUnit = 1545772 + 262144;  // input bytes + output bytes

WorkloadConfig config_of(std::uint64_t footprint, std::uint32_t decomposition = 0,
                         simd::IsaPath isa = simd::IsaPath::kScalar) {
  return WorkloadConfig{9, footprint, decomposition, isa};
}

std::unique_ptr<Workload> made(const WorkloadConfig& config) {
  auto created = create(config);
  EXPECT_TRUE(created.has_value()) << created.error().detail;
  return created.has_value() ? std::move(created).value() : nullptr;
}

TEST(SparseCreate, TheUnitHasTheSizesTheSpecificationImplies) {
  static_assert(kNonZeros == 112430);
  static_assert(kInputBytes == 1545772);
  static_assert(kOutputBytes == 262144);
  const auto workload = made(config_of(5 * kUnit));
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, 1545772U);
  EXPECT_EQ(plan.output_bytes, 262144U);
  EXPECT_EQ(plan.units, 5U);
  EXPECT_EQ(plan.decomposition, 1024U) << "the default, resolved";
  // x, r, p, q (4 * 16384 doubles), 64 chunk partials, and 8 bytes of alignment slack.
  EXPECT_EQ(plan.scratch_bytes, (4 * 16384 + 64) * 8 + 8U);
}

TEST(SparseCreate, TheLogicalTrafficAndTheOperationsAreTheAlgorithmsOwn) {
  const auto workload = made(config_of(kUnit));
  ASSERT_NE(workload, nullptr);
  // One SpMV: 112430 * (8 + 4 + 8) bytes and 16385 * 4 of row_ptr.
  constexpr std::uint64_t spmv = 112430 * 20 + 16385 * 4;
  constexpr std::uint64_t vector = 16384 * 8;
  constexpr std::uint64_t expected_bytes =
      5 * vector + 16 * (spmv + 13 * vector) + (spmv + 3 * vector);
  EXPECT_EQ(expected_bytes, 67651932U) << "the arithmetic above, written once as a number";
  EXPECT_EQ(workload->plan().logical_bytes, expected_bytes);
  constexpr std::uint64_t expected_operations =
      2 * 16384 + 16 * (2 * 112430 + 10 * 16384) + 2 * 112430;
  EXPECT_EQ(expected_operations, 6476828U);
  EXPECT_EQ(workload->plan().operations, expected_operations);
}

TEST(SparseCreate, UnitsAreWhatTheFootprintHoldsAndOneUnitIsTheLeast) {
  EXPECT_EQ(made(config_of(kUnit))->plan().units, 1U);
  EXPECT_EQ(made(config_of(2 * kUnit - 1))->plan().units, 1U);
  EXPECT_EQ(made(config_of(2 * kUnit))->plan().units, 2U);
  for (const std::uint64_t footprint : {std::uint64_t{0}, std::uint64_t{1}, kUnit - 1}) {
    const auto refused = create(config_of(footprint));
    ASSERT_FALSE(refused.has_value()) << footprint;
    EXPECT_EQ(refused.error().fault, WorkloadFault::kFootprintTooSmall);
    EXPECT_NE(refused.error().detail.find(std::to_string(footprint)), std::string::npos)
        << "the detail names the footprint";
    EXPECT_NE(refused.error().detail.find("1807916"), std::string::npos)
        << "and the size of the unit it does not hold";
  }
}

TEST(SparseCreate, TheDescriptorIsTheSpecificationsAndOffersScalarOnly) {
  const auto workload = made(config_of(kUnit));
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "sparse");
  EXPECT_EQ(descriptor.version, 1U);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_EQ(descriptor.paths, simd::IsaSet{simd::IsaPath::kScalar});
}

TEST(SparseCreate, EachSpecifiedChunkSizeIsAcceptedAndNoOtherIs) {
  for (const std::uint32_t rows : kChunkRows) {
    EXPECT_EQ(made(config_of(kUnit, rows))->plan().decomposition, rows);
  }
  for (const std::uint32_t rows : {1U, 128U, 255U, 1023U, 1025U, 4096U, 16384U, 0xFFFFFFFFU}) {
    const auto refused = create(config_of(kUnit, rows));
    ASSERT_FALSE(refused.has_value()) << rows;
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::to_string(rows)), std::string::npos)
        << "the detail names the value";
  }
}

TEST(SparseCreate, APathWithoutKernelsIsRefusedByName) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    const auto refused = create(config_of(kUnit, 0, path));
    ASSERT_FALSE(refused.has_value()) << simd::describe(path);
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::string(simd::describe(path))), std::string::npos);
  }
  const auto outside = create(config_of(kUnit, 0, static_cast<simd::IsaPath>(200)));
  ASSERT_FALSE(outside.has_value()) << "a value outside the enumeration is no path";
  EXPECT_EQ(outside.error().fault, WorkloadFault::kConfiguration);
}

TEST(SparseCreate, ThePathIsRefusedBeforeTheFootprintIsLookedAt) {
  const auto refused = create(config_of(0, 0, simd::IsaPath::kAvx2));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
}

}  // namespace
}  // namespace loadforge::workloads::sparse
