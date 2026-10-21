// SPDX-License-Identifier: GPL-3.0-or-later
//
// The adaptive mesh workload at T1: WorkloadContract, then what the contract
// leaves to the workload -- the descriptor, the plan's figures and their
// footprint boundaries, each refusal at creation and what it names, the
// generated hotspots, and compute() on hand-made inputs whose meshes are
// known: no hotspot in the lattice, one in a corner, one per coarse cell
// (the leaf table exactly full), a cell crowded to its limit, and the
// hotspots of a generated unit.

#include "workloads/workload.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/adaptive_mesh/support.hpp"
#include "verification/record.hpp"
#include "workloads/adaptive_mesh/adaptive_mesh.hpp"
#include "workloads/adaptive_mesh/spec.hpp"

namespace loadforge::workloads::adaptive_mesh {
namespace {

using simd::IsaPath;
using testing::Bytes;
using testing::Hotspots;

constexpr std::uint64_t kUnit = std::uint64_t{kInputBytes} + kOutputBytes;

// ---- WorkloadContract ---------------------------------------------------------

struct Subject {
  static constexpr const char* kName = "adaptive_mesh";
  static WorkloadConfig config() { return testing::config_for(); }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return adaptive_mesh::create(c);
  }
};

using Subjects = ::testing::Types<Subject>;

// ---- the descriptor and the plan -----------------------------------------------

TEST(AdaptiveMeshDescriptor, NamesTheWorkloadItsClassAndItsContract) {
  const auto workload = testing::make(testing::config_for());
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "adaptive_mesh");
  EXPECT_EQ(descriptor.name, kName);
  EXPECT_EQ(descriptor.version, kWorkloadVersion);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBoundedResidual);
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_EQ(descriptor.paths, simd::IsaSet{IsaPath::kScalar});
}

TEST(AdaptiveMeshPlan, TheFiguresOfOneUnit) {
  const auto workload = testing::make(testing::config_for());
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, kInputBytes);
  EXPECT_EQ(plan.input_bytes, 2048U);
  EXPECT_EQ(plan.output_bytes, kOutputBytes);
  EXPECT_EQ(plan.output_bytes, 3072U);
  EXPECT_EQ(plan.scratch_bytes, 2048U) << "one partial sum per coarse cell, per slot";
  EXPECT_EQ(plan.units, 2U);
  EXPECT_EQ(plan.decomposition, 1U) << "no decomposition: the plan reports 1";
  EXPECT_EQ(plan.logical_bytes, kInputBytes + kOutputBytes);
  EXPECT_EQ(plan.operations, kTypicalLeaves * kHotspots * (kOperationsPerTerm + 1));
}

TEST(AdaptiveMeshPlan, TheLeafTableIsExactlyTheMostAnInputCanMake) { EXPECT_EQ(kMaxLeaves, 4096U); }

TEST(AdaptiveMeshPlan, TheFootprintAtEachSideOfOneUnitAndOfTwo) {
  for (const auto& [footprint, units] : {std::pair<std::uint64_t, std::uint64_t>{kUnit, 1},
                                         {(2 * kUnit) - 1, 1},
                                         {2 * kUnit, 2},
                                         {(3 * kUnit) + 5, 3},
                                         {std::numeric_limits<std::uint64_t>::max(),
                                          std::numeric_limits<std::uint64_t>::max() / kUnit}}) {
    const auto workload = testing::make(WorkloadConfig{1, footprint, 0, IsaPath::kScalar});
    ASSERT_NE(workload, nullptr) << footprint;
    EXPECT_EQ(workload->plan().units, units) << footprint;
  }
  const auto refused = create(WorkloadConfig{1, kUnit - 1, 0, IsaPath::kScalar});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(refused.error().detail.find(std::to_string(kUnit - 1)), std::string::npos);
  EXPECT_NE(refused.error().detail.find(std::to_string(kUnit)), std::string::npos);
  const auto empty = create(WorkloadConfig{1, 0, 0, IsaPath::kScalar});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().fault, WorkloadFault::kFootprintTooSmall);
}

TEST(AdaptiveMeshCreate, DecompositionZeroAndOneAreTheSameAndOthersAreRefusedByValue) {
  for (const std::uint32_t decomposition : {0U, 1U}) {
    WorkloadConfig config = testing::config_for();
    config.decomposition = decomposition;
    const auto workload = testing::make(config);
    ASSERT_NE(workload, nullptr);
    EXPECT_EQ(workload->plan().decomposition, 1U);
  }
  for (const std::uint32_t decomposition : {2U, 16U, 0xFFFFFFFFU}) {
    WorkloadConfig config = testing::config_for();
    config.decomposition = decomposition;
    const auto refused = create(config);
    ASSERT_FALSE(refused.has_value()) << decomposition;
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::to_string(decomposition)), std::string::npos);
  }
}

TEST(AdaptiveMeshCreate, AnIsaPathOtherThanScalarIsRefusedByName) {
  for (const IsaPath path : {IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon}) {
    WorkloadConfig config = testing::config_for();
    config.isa = path;
    const auto refused = create(config);
    ASSERT_FALSE(refused.has_value()) << simd::describe(path);
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::string(simd::describe(path))), std::string::npos);
  }
}

// ---- generate -------------------------------------------------------------------

TEST(AdaptiveMeshGenerate, EachHotspotIsTheDrawOfItsIndexAndLiesInTheLattice) {
  const auto workload = testing::make(testing::config_for(7));
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t unit : {std::uint64_t{0}, std::uint64_t{3}, ~std::uint64_t{0}}) {
    const Bytes input = testing::generated(*workload, unit);
    ASSERT_EQ(input.size(), kInputBytes);
    for (std::size_t h = 0; h < kHotspots; ++h) {
      const std::uint64_t draw = rng::u64_of(rng::block_at(7, unit, h));
      const Hotspot expected = hotspot_of(draw);
      const Hotspot got = testing::hotspot_in(input, h);
      EXPECT_EQ(got.x, expected.x);
      EXPECT_EQ(got.y, expected.y);
      EXPECT_EQ(got.x, static_cast<std::uint32_t>(draw >> 55U));
      EXPECT_EQ(got.y, static_cast<std::uint32_t>((draw >> 46U) & 0x1FFU));
      EXPECT_LT(got.x, kLattice);
      EXPECT_LT(got.y, kLattice);
    }
  }
}

TEST(AdaptiveMeshGenerate, TheBytesAreLittleEndianWords) {
  const auto workload = testing::make(testing::config_for(7));
  ASSERT_NE(workload, nullptr);
  const Bytes input = testing::generated(*workload, 1);
  const Hotspot first = hotspot_of(rng::u64_of(rng::block_at(7, 1, 0)));
  EXPECT_EQ(input[0], static_cast<std::byte>(first.x & 0xFFU));
  EXPECT_EQ(input[1], static_cast<std::byte>(first.x >> 8U));
  EXPECT_EQ(input[2], std::byte{0});
  EXPECT_EQ(input[3], std::byte{0});
  EXPECT_EQ(input[4], static_cast<std::byte>(first.y & 0xFFU));
  EXPECT_EQ(input[5], static_cast<std::byte>(first.y >> 8U));
}

// ---- compute --------------------------------------------------------------------

/// Output of `spots` on the serial executor, with the whole of it checked
/// against the reference.
Bytes checked(const Hotspots& spots) {
  const auto workload = testing::make(testing::config_for());
  EXPECT_NE(workload, nullptr);
  testing::Serial serial;
  const Bytes output = testing::computed(*workload, 0, testing::input_of(spots), serial);
  EXPECT_EQ(testing::first_disagreement(output, spots), "");
  return output;
}

TEST(AdaptiveMeshCompute, NoHotspotInTheLatticeLeavesTheBaseMeshAndItsIntegralsAreZeroTerms) {
  // Every coordinate at or above kLattice is in no cell: 256 leaves, one per coarse cell.
  for (const Hotspot outside : {Hotspot{kLattice, 0}, Hotspot{0, kLattice},
                                Hotspot{0xFFFFFFFFU, 0xFFFFFFFFU}, Hotspot{kLattice, kLattice}}) {
    const Bytes output = checked(testing::crowd(outside));
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      EXPECT_EQ(testing::count_of(output, cell), 1U);
      EXPECT_GT(testing::integral_of(output, cell), 0.0);
    }
  }
}

TEST(AdaptiveMeshCompute, AHotspotInACornerRefinesItsCellDownToTheFinestLevel) {
  const Bytes output = checked(testing::crowd({0, 0}));
  EXPECT_EQ(testing::count_of(output, 0), 16U) << "1 + 3 for each of 5 splits";
  for (std::size_t cell = 1; cell < kCoarseCells; ++cell) {
    EXPECT_EQ(testing::count_of(output, cell), 1U);
  }
  const Bytes far = checked(testing::crowd({kLattice - 1, kLattice - 1}));
  EXPECT_EQ(testing::count_of(far, kCoarseCells - 1), 16U);
  const Bytes edge = checked(testing::crowd({kLattice - 1, 0}));
  EXPECT_EQ(testing::count_of(edge, kBaseCells - 1), 16U);
}

TEST(AdaptiveMeshCompute, OneHotspotPerCoarseCellFillsTheLeafTableExactly) {
  Hotspots spots;
  constexpr std::uint32_t kSide = kLattice / kBaseCells;
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    spots.push_back({static_cast<std::uint32_t>(cell % kBaseCells) * kSide + 5,
                     static_cast<std::uint32_t>(cell / kBaseCells) * kSide + 17});
  }
  const Bytes output = checked(spots);
  std::uint32_t total = 0;
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    EXPECT_EQ(testing::count_of(output, cell), 16U);
    total += testing::count_of(output, cell);
  }
  EXPECT_EQ(total, kMaxLeaves);
}

TEST(AdaptiveMeshCompute, ACrowdedCellHoldsManyLeavesAndNoMoreThanItsLimit) {
  // 256 distinct finest cells of coarse cell 0 (32 x 32 of them), one in four.
  Hotspots spots;
  for (std::uint32_t y = 0; y < 32; y += 2) {
    for (std::uint32_t x = 0; x < 32; x += 2) {
      spots.push_back({x, y});
    }
  }
  ASSERT_EQ(spots.size(), kHotspots);
  const Bytes output = checked(spots);
  const std::uint32_t leaves = testing::count_of(output, 0);
  EXPECT_GT(leaves, 700U);
  EXPECT_LE(leaves, kMaxLeavesPerCell);
}

TEST(AdaptiveMeshCompute, HotspotsMayShareACellAndOrder) {
  // The same two hotspots in either order, 128 times each: the same mesh.
  Hotspots forward;
  Hotspots backward;
  for (std::size_t h = 0; h < kHotspots / 2; ++h) {
    forward.push_back({40, 40});
    forward.push_back({300, 100});
    backward.push_back({300, 100});
    backward.push_back({40, 40});
  }
  const Bytes one = checked(forward);
  const Bytes two = checked(backward);
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    EXPECT_EQ(testing::count_of(one, cell), testing::count_of(two, cell));
  }
}

TEST(AdaptiveMeshCompute, GeneratedUnitsAgreeWithTheReferenceInEveryCell) {
  const auto workload = testing::make(testing::config_for(11, 8));
  ASSERT_NE(workload, nullptr);
  testing::Serial serial;
  for (std::uint64_t unit = 0; unit < 4; ++unit) {
    const Bytes input = testing::generated(*workload, unit);
    const Bytes output = testing::computed(*workload, unit, input, serial);
    EXPECT_EQ(testing::first_disagreement(output, testing::hotspots_of(input)), "")
        << "unit " << unit;
  }
}

TEST(AdaptiveMeshCompute, TheAnswerIsAFunctionOfTheInputAndNotOfTheUnitIndex) {
  const auto workload = testing::make(testing::config_for(11, 8));
  ASSERT_NE(workload, nullptr);
  testing::Serial serial;
  const Bytes input = testing::generated(*workload, 2);
  EXPECT_EQ(testing::computed(*workload, 2, input, serial),
            testing::computed(*workload, 6, input, serial));
}

TEST(AdaptiveMeshCompute, AnOutputTrailingByteNeverReadsOrWritesBeyondTheSpans) {
  const auto workload = testing::make(testing::config_for());
  ASSERT_NE(workload, nullptr);
  testing::Serial serial;
  const Bytes input = testing::input_of(testing::crowd({3, 3}));
  // Spans cut out of larger buffers with guard bytes around them.
  Bytes in_guard(kInputBytes + 16, std::byte{0xA5});
  std::copy(input.begin(), input.end(), in_guard.begin() + 8);
  Bytes out_guard(kOutputBytes + 16, std::byte{0x5A});
  Bytes scratch_guard(kScratchBytes + 16, std::byte{0x3C});
  workload->compute(0, std::span<const std::byte>(in_guard).subspan(8, kInputBytes),
                    std::span<std::byte>(out_guard).subspan(8, kOutputBytes),
                    std::span<std::byte>(scratch_guard).subspan(8, kScratchBytes), serial);
  for (std::size_t at = 0; at < 8; ++at) {
    EXPECT_EQ(out_guard[at], std::byte{0x5A});
    EXPECT_EQ(out_guard[kOutputBytes + 8 + at], std::byte{0x5A});
    EXPECT_EQ(scratch_guard[at], std::byte{0x3C});
    EXPECT_EQ(scratch_guard[kScratchBytes + 8 + at], std::byte{0x3C});
  }
  const Bytes expected = testing::computed(*workload, 0, input, serial);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out_guard.begin() + 8));
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh

namespace loadforge::contracts {
// The registry that links every workload is the integration stream's, and
// runs RegistryContract there.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);
INSTANTIATE_TYPED_TEST_SUITE_P(AdaptiveMesh, WorkloadContract, workloads::adaptive_mesh::Subjects);
}  // namespace loadforge::contracts
