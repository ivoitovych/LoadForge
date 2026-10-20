// SPDX-License-Identifier: GPL-3.0-or-later
//
// The workload against its contract suite, and what it adds to the contract:
// the factory's refusals, the plan's numbers and the generated input.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/particles/support.hpp"
#include "workloads/particles/grid.hpp"
#include "workloads/particles/kernels.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

TEST(ParticlesWorkload, TheDescriptorSaysWhatTheSpecificationSays) {
  const auto workload = testing::make(testing::config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "particles");
  EXPECT_EQ(descriptor.version, 1U);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx512));
#if defined(LOADFORGE_PARTICLES_AVX2)
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kAvx2));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kNeon));
#elif defined(LOADFORGE_PARTICLES_NEON)
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kNeon));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx2));
#else
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx2));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kNeon));
#endif
  EXPECT_EQ(descriptor.paths, simd::kernel_paths(cell_kernels()));
}

TEST(ParticlesWorkload, TheDefaultDecompositionIsTwoCellsPerCutoff) {
  const auto workload = testing::make(testing::config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->plan().decomposition, kDefaultCellsPerCutoff);
  for (const std::uint32_t c : kCellsPerCutoff) {
    const auto chosen = testing::make(testing::config_for(c, simd::IsaPath::kScalar));
    ASSERT_NE(chosen, nullptr) << c;
    EXPECT_EQ(chosen->plan().decomposition, c);
  }
}

TEST(ParticlesWorkload, ADecompositionThatIsNotOneTwoOrFourIsRefusedByName) {
  for (const std::uint32_t cells : {3U, 5U, 8U, 16U, 1U << 31U}) {
    const auto created = create(testing::config_for(cells, simd::IsaPath::kScalar));
    ASSERT_FALSE(created.has_value()) << cells;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(cells)), std::string::npos)
        << created.error().detail;
  }
}

TEST(ParticlesWorkload, AnIsaPathWithoutAKernelIsRefusedByName) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    const auto probe = testing::make(testing::config_for(0, simd::IsaPath::kScalar));
    if (probe->descriptor().paths.contains(path)) {
      continue;
    }
    const auto created = create(testing::config_for(0, path));
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::string(simd::describe(path))), std::string::npos)
        << created.error().detail;
  }
}

TEST(ParticlesWorkload, AnIsaPathOutsideTheEnumerationIsRefusedLikeOneWithoutAKernel) {
  // A value no worker would pass: the kernel table has no entry for it, and
  // the table's lookup says so (dispatch.hpp, kernel_at) rather than guessing.
  for (const auto raw : {std::uint8_t{4}, std::uint8_t{200}, std::uint8_t{255}}) {
    const auto created = create(testing::config_for(0, static_cast<simd::IsaPath>(raw)));
    ASSERT_FALSE(created.has_value()) << static_cast<int>(raw);
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find("unrecognised"), std::string::npos)
        << created.error().detail;
  }
}

TEST(ParticlesWorkload, TheFootprintDecidesTheUnitsAndOneUnitIsTheLeastItTakes) {
  constexpr std::uint64_t kUnit = kInputBytes + kOutputBytes;
  EXPECT_EQ(kUnit, 196608U);
  WorkloadConfig config = testing::config_for(0, simd::IsaPath::kScalar);
  config.footprint_bytes = kUnit - 1;
  const auto short_by_one = create(config);
  ASSERT_FALSE(short_by_one.has_value());
  EXPECT_EQ(short_by_one.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(short_by_one.error().detail.find(std::to_string(kUnit - 1)), std::string::npos);
  config.footprint_bytes = 0;
  const auto none = create(config);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, WorkloadFault::kFootprintTooSmall);
  for (const std::uint64_t footprint : {kUnit, 2 * kUnit - 1, 2 * kUnit, 11 * kUnit + 5}) {
    config.footprint_bytes = footprint;
    const auto created = testing::make(config);
    ASSERT_NE(created, nullptr) << footprint;
    EXPECT_EQ(created->plan().units, footprint / kUnit);
  }
}

TEST(ParticlesWorkload, ARefusalOfTheDecompositionComesBeforeOneOfTheFootprint) {
  WorkloadConfig config = testing::config_for(3, simd::IsaPath::kScalar);
  config.footprint_bytes = 1;
  const auto created = create(config);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
}

// The cells a particle looks at, counted from the geometry: the offsets in
// [-c, c]^3 whose nearest points are closer than the cutoff.
std::uint64_t cells_visited(int c) {
  const auto gap = [](int d) { return std::max(std::abs(d) - 1, 0); };
  std::uint64_t visited = 0;
  for (int dz = -c; dz <= c; ++dz) {
    for (int dy = -c; dy <= c; ++dy) {
      for (int dx = -c; dx <= c; ++dx) {
        if (gap(dx) * gap(dx) + gap(dy) * gap(dy) + gap(dz) * gap(dz) < c * c) {
          ++visited;
        }
      }
    }
  }
  return visited;
}

TEST(ParticlesWorkload, ThePlanStatesSizesAndWork) {
  struct Expected {
    std::uint32_t c;
    std::uint64_t cells;
  };
  for (const Expected expected : {Expected{1, 512}, Expected{2, 4096}, Expected{4, 32768}}) {
    const auto workload = testing::make(testing::config_for(expected.c, simd::IsaPath::kScalar));
    ASSERT_NE(workload, nullptr);
    const Plan& plan = workload->plan();
    EXPECT_EQ(plan.input_bytes, kInputBytes);
    EXPECT_EQ(plan.output_bytes, kOutputBytes);
    EXPECT_EQ(plan.units, 4U);
    const Layout layout = make_layout(make_grid(expected.c));
    EXPECT_EQ(plan.scratch_bytes, layout.total + kScratchAlignment);
    EXPECT_EQ(plan.logical_bytes, kInputBytes + kOutputBytes + 2 * layout.total);
    // 17 operations for each candidate pair; kParticles / cells candidates in
    // a cell on average, in each of the cells visited.
    EXPECT_EQ(plan.operations, kParticles * kParticles *
                                   cells_visited(static_cast<int>(expected.c)) / expected.cells *
                                   17U);
  }
  EXPECT_EQ(cells_visited(1), 27U);
  EXPECT_EQ(cells_visited(2), 125U);
  EXPECT_EQ(cells_visited(4), 613U);
}

std::uint64_t le64(const Bytes& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8 * byte);
  }
  return value;
}

TEST(ParticlesWorkload, ElementThreePPlusCIsThePositionOfTheDrawAtThatBlock) {
  const WorkloadConfig config = testing::config_for(0, simd::IsaPath::kScalar, 0x1234ABCD5678EF01);
  const auto workload = testing::make(config);
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{9}, ~std::uint64_t{0}}) {
    const Bytes input = testing::generate(*workload, index);
    ASSERT_EQ(input.size(), kInputBytes);
    for (const std::size_t element :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
          std::size_t{1000}, 3 * kParticles - 3, 3 * kParticles - 2, 3 * kParticles - 1}) {
      // The generator's first two words, word 0 low, then the specification's
      // (bits >> 11) * 2^-49, with the double read back byte by byte.
      const rng::Block block = rng::block_at(config.seed, index, element);
      const std::uint64_t bits = std::uint64_t{block[0]} | (std::uint64_t{block[1]} << 32U);
      const double expected = static_cast<double>(bits >> 11U) * std::ldexp(1.0, -49);
      EXPECT_EQ(std::bit_cast<double>(le64(input, 8 * element)), expected)
          << "unit " << index << " element " << element;
    }
  }
}

TEST(ParticlesWorkload, EveryGeneratedCoordinateIsAMultipleOfTwoToMinus49BelowTheBoxEdge) {
  const auto workload = testing::make(testing::config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const std::vector<double> coordinates = testing::as_doubles(testing::generate(*workload, 3));
  ASSERT_EQ(coordinates.size(), 3 * kParticles);
  for (const double coordinate : coordinates) {
    ASSERT_GE(coordinate, 0.0);
    ASSERT_LT(coordinate, kBoxEdge);
    ASSERT_EQ(std::ldexp(std::nearbyint(std::ldexp(coordinate, 49)), -49), coordinate);
  }
  // Spread over the whole box, so a generator stuck in a corner is seen.
  EXPECT_GT(*std::max_element(coordinates.begin(), coordinates.end()), 15.0);
  EXPECT_LT(*std::min_element(coordinates.begin(), coordinates.end()), 1.0);
}

TEST(ParticlesWorkload, TheSeedAndTheIndexEachSelectTheData) {
  const auto a = testing::make(testing::config_for(0, simd::IsaPath::kScalar, 1));
  const auto b = testing::make(testing::config_for(0, simd::IsaPath::kScalar, 2));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(testing::generate(*a, 0), testing::generate(*b, 0));
  EXPECT_NE(testing::generate(*a, 0), testing::generate(*a, 1));
  EXPECT_EQ(testing::generate(*a, 5), testing::generate(*a, 5));
}

}  // namespace
}  // namespace loadforge::workloads::particles

namespace loadforge::contracts {
namespace {

namespace particles = ::loadforge::workloads::particles;

// The contract header registers the registry suite too; a binary that
// instantiates only the workload suite must say so.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);

template <std::uint32_t kCells, bool kVector>
struct Subject {
  static constexpr std::string_view kName = "particles";
  static simd::IsaPath path() {
    // The vector subject runs the native path where this machine has it, and
    // the scalar path where it does not (a machine without AVX2 cannot run
    // the AVX2 kernel; the worker would not pick it).
    const auto paths = particles::testing::runnable_paths();
    return kVector ? paths.back() : simd::IsaPath::kScalar;
  }
  static workloads::WorkloadConfig config() {
    return particles::testing::config_for(kCells, path());
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return workloads::particles::create(config);
  }
};

using Subjects = ::testing::Types<Subject<0, false>, Subject<1, false>, Subject<4, false>,
                                  Subject<2, true>, Subject<4, true>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Particles, WorkloadContract, Subjects);

}  // namespace
}  // namespace loadforge::contracts
