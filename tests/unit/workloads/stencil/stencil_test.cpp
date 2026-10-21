// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stencil workload against the workload contract, and the facts of its
// plan, its refusals, its generator and the edges of its input domain.

#include "workloads/stencil/stencil.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/stencil/stencil_support.hpp"
#include "workloads/stencil/kernels.hpp"
#include "workloads/stencil/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;

}  // namespace
}  // namespace loadforge::workloads::stencil

namespace loadforge::contracts {

namespace {

template <std::uint32_t Tile, bool Widest>
struct StencilSubject {
  static constexpr std::string_view kName = "stencil";
  static workloads::WorkloadConfig config() {
    return workloads::stencil::testing::config_for(
        Widest ? workloads::stencil::testing::widest_runnable_path() : simd::IsaPath::kScalar,
        Tile);
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return workloads::stencil::create(config);
  }
};

using StencilSubjects =
    ::testing::Types<StencilSubject<0, false>, StencilSubject<16, false>, StencilSubject<32, true>,
                     StencilSubject<64, true>, StencilSubject<128, false>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Stencil, WorkloadContract, StencilSubjects);

}  // namespace
}  // namespace loadforge::contracts

namespace loadforge::workloads::stencil {
namespace {

TEST(StencilDescriptor, SaysWhatTheSpecificationDeclares) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "stencil");
  EXPECT_EQ(descriptor.version, kVersion);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(describe(descriptor.determinism), "BitExactFixedDecomposition");
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx512));
  EXPECT_EQ(descriptor.paths, simd::kernel_paths(kernel_table()));
}

TEST(StencilPlan, IsTheGeometryOfOneUnit) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, 0, 5));
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, 524288U);
  EXPECT_EQ(plan.output_bytes, 524288U);
  EXPECT_EQ(plan.scratch_bytes, 524288U) << "the grid the sweeps alternate with";
  EXPECT_EQ(plan.units, 5U);
  EXPECT_EQ(plan.decomposition, kDefaultTileEdge);
  EXPECT_EQ(plan.logical_bytes, 8U * 2U * 524288U) << "each sweep reads a grid and writes one";
  EXPECT_EQ(plan.operations, 8U * 254U * 254U * 6U);
}

TEST(StencilPlan, TheFootprintFloorsToWholeUnits) {
  WorkloadConfig config = testing::config_for(simd::IsaPath::kScalar, 0, 3);
  config.footprint_bytes += 1048575;  // one byte short of a fourth unit
  EXPECT_EQ(testing::make(config)->plan().units, 3U);
  config.footprint_bytes += 1;
  EXPECT_EQ(testing::make(config)->plan().units, 4U);
  config.footprint_bytes = 1048576 - 1;
  const auto refused = create(config);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(refused.error().detail.find("1048575"), std::string::npos);
  config.footprint_bytes = 1048576;
  EXPECT_EQ(testing::make(config)->plan().units, 1U) << "exactly one unit fits";
}

TEST(StencilPlan, ADecompositionIsATileEdgeOrZero) {
  for (const std::uint32_t tile : {16U, 32U, 64U, 128U}) {
    const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, tile));
    ASSERT_NE(workload, nullptr) << tile;
    EXPECT_EQ(workload->plan().decomposition, tile);
  }
  for (const std::uint32_t tile :
       {1U, 8U, 15U, 17U, 48U, 63U, 65U, 129U, 256U, 512U, 4294967295U}) {
    const auto refused = create(testing::config_for(simd::IsaPath::kScalar, tile));
    ASSERT_FALSE(refused.has_value()) << tile;
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::to_string(tile)), std::string::npos) << tile;
  }
}

TEST(StencilPlan, APathWithoutAKernelIsRefusedByName) {
  const simd::IsaSet have = simd::kernel_paths(kernel_table());
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    const auto created = create(testing::config_for(path));
    EXPECT_EQ(created.has_value(), have.contains(path)) << simd::describe(path);
    if (!created.has_value()) {
      EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
      EXPECT_NE(created.error().detail.find(std::string(simd::describe(path))), std::string::npos);
    }
  }
}

TEST(StencilPlan, APathOutsideTheEnumerationIsRefusedAsOneWithNoKernel) {
  const auto created = create(testing::config_for(static_cast<simd::IsaPath>(200)));
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
  EXPECT_NE(created.error().detail.find("unrecognised"), std::string::npos);
  EXPECT_EQ(simd::kernel_at(kernel_table(), static_cast<simd::IsaPath>(200)), nullptr);
}

TEST(StencilPlan, ADecompositionIsJudgedBeforeTheFootprint) {
  WorkloadConfig config = testing::config_for(simd::IsaPath::kScalar, 48);
  config.footprint_bytes = 1;
  const auto refused = create(config);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
}

TEST(StencilGenerate, IsTheSpecificationsInputBitForBit) {
  for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{0x5EED}, ~std::uint64_t{0}}) {
    const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, 0, 4, seed));
    ASSERT_NE(workload, nullptr);
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{1} << 40}) {
      EXPECT_EQ(testing::generated(*workload, index), testing::spec_input(seed, index))
          << seed << "/" << index;
    }
  }
}

TEST(StencilGenerate, EveryCellIsFiniteWithinOneAndAMultipleOfTwoToMinus52) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  const Bytes input = testing::generated(*workload, 7);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    const double value = testing::cell_of(input, cell / kEdge, cell % kEdge);
    ASSERT_GE(value, -1.0);
    ASSERT_LT(value, 1.0);
    ASSERT_EQ(std::ldexp(value, 52), std::trunc(std::ldexp(value, 52)));
  }
}

TEST(StencilGenerate, IsLittleEndianIeeeBytes) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  const Bytes input = testing::generated(*workload, 1);
  const double first = entry_value(rng::u64_of(rng::block_at(0x5EED, 1, 0)));
  const auto bits = std::bit_cast<std::uint64_t>(first);
  for (std::size_t byte = 0; byte < 8; ++byte) {
    EXPECT_EQ(std::to_integer<std::uint64_t>(input[byte]), (bits >> (8 * byte)) & 0xFFU) << byte;
  }
}

TEST(StencilCompute, KeepsTheRingBitForBitWhateverItHolds) {
  // A ring of NaN payloads, infinities, negative zero and a subnormal: copied,
  // not computed on.
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, 32));
  Bytes input = testing::generated(*workload, 2);
  const std::uint64_t specials[] = {0x7FF8DEADBEEF1234ULL, 0xFFF0000000000000ULL,
                                    0x8000000000000000ULL, 0x0000000000000001ULL,
                                    0x7FF0000000000000ULL};
  std::size_t next = 0;
  for (std::size_t at = 0; at < kEdge; ++at) {
    for (const auto& [row, column] :
         {std::pair{std::size_t{0}, at}, {kEdge - 1, at}, {at, std::size_t{0}}, {at, kEdge - 1}}) {
      std::memcpy(input.data() + (row * kEdge + column) * 8, &specials[next++ % 5], 8);
    }
  }
  testing::SerialExecutor serial;
  const Bytes output = testing::computed(*workload, 2, input, serial);
  for (std::size_t at = 0; at < kEdge; ++at) {
    for (const auto& [row, column] :
         {std::pair{std::size_t{0}, at}, {kEdge - 1, at}, {at, std::size_t{0}}, {at, kEdge - 1}}) {
      EXPECT_EQ(std::memcmp(input.data() + (row * kEdge + column) * 8,
                            output.data() + (row * kEdge + column) * 8, 8),
                0)
          << row << "," << column;
    }
  }
}

TEST(StencilCompute, ANotANumberSpreadsExactlyAsFarAsEightSweepsReach) {
  // A zero grid with one NaN: after kSteps sweeps a cell is NaN when it is
  // within Manhattan distance kSteps of the NaN, and zero otherwise.
  const std::size_t centre = 128;
  for (const std::uint32_t tile : {16U, 128U}) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(path, tile));
      Bytes input = testing::grid_of(0.0);
      testing::set_cell(input, centre, centre, std::numeric_limits<double>::quiet_NaN());
      testing::SerialExecutor serial;
      const Bytes output = testing::computed(*workload, 0, input, serial);
      for (std::size_t row = 0; row < kEdge; ++row) {
        for (std::size_t column = 0; column < kEdge; ++column) {
          const std::size_t distance = (row > centre ? row - centre : centre - row) +
                                       (column > centre ? column - centre : centre - column);
          const double value = testing::cell_of(output, row, column);
          ASSERT_EQ(std::isnan(value), distance <= kSteps)
              << row << "," << column << " tile " << tile << " " << simd::describe(path);
          if (distance > kSteps) {
            ASSERT_EQ(value, 0.0);
          }
        }
      }
    }
  }
}

TEST(StencilCompute, InfinityIsInputLikeAnyOther) {
  // +inf beside -inf becomes NaN, +inf alone stays +inf where it reaches.
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  Bytes input = testing::grid_of(0.0);
  testing::set_cell(input, 100, 100, std::numeric_limits<double>::infinity());
  testing::SerialExecutor serial;
  Bytes output = testing::computed(*workload, 0, input, serial);
  EXPECT_EQ(testing::cell_of(output, 100, 100), std::numeric_limits<double>::infinity());
  EXPECT_EQ(testing::cell_of(output, 100, 108), std::numeric_limits<double>::infinity());
  EXPECT_EQ(testing::cell_of(output, 100, 109), 0.0);
  testing::set_cell(input, 100, 101, -std::numeric_limits<double>::infinity());
  output = testing::computed(*workload, 0, input, serial);
  EXPECT_TRUE(std::isnan(testing::cell_of(output, 100, 100)));
  EXPECT_TRUE(std::isnan(testing::cell_of(output, 100, 101)));
}

TEST(StencilCompute, AConstantGridIsAFixedPointExactly) {
  for (const double value : {0.0, 1.0, -1.0, 0.1, -0.7071067811865476, 1.0 - 0x1p-52}) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(path));
      testing::SerialExecutor serial;
      const Bytes input = testing::grid_of(value);
      EXPECT_EQ(testing::computed(*workload, 0, input, serial), input)
          << value << " " << simd::describe(path);
    }
  }
}

TEST(StencilCompute, TheUnitIndexDoesNotEnterTheComputation) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  const Bytes input = testing::generated(*workload, 1);
  testing::SerialExecutor serial;
  EXPECT_EQ(testing::computed(*workload, 1, input, serial),
            testing::computed(*workload, 99, input, serial));
}

}  // namespace
}  // namespace loadforge::workloads::stencil
