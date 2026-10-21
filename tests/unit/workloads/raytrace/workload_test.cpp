// SPDX-License-Identifier: GPL-3.0-or-later
//
// The factory, the descriptor and the plan, and the layout of what compute()
// writes: what a configuration is refused for, what an accepted one resolves
// to, and every byte of the output accounted for.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/raytrace.hpp"
#include "workloads/raytrace/spec.hpp"

namespace loadforge::tests::raytrace {
namespace {

constexpr std::uint64_t kUnitBytes = rt::kInputBytes + rt::kOutputBytes;

TEST(RaytraceFactory, ADecompositionThatIsNotATileEdgeIsRefusedAndNamed) {
  for (const std::uint32_t edge : {1U, 4U, 7U, 12U, 24U, 33U, 64U, 4096U, 0xFFFFFFFFU}) {
    const auto created = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, edge));
    ASSERT_FALSE(created.has_value()) << edge;
    EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(edge)), std::string::npos)
        << created.error().detail;
  }
}

TEST(RaytraceFactory, EveryTileEdgeAndZeroForTheDefaultAreAccepted) {
  for (const std::uint32_t edge : {0U, 8U, 16U, 32U}) {
    const auto created = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, edge));
    ASSERT_TRUE(created.has_value()) << edge;
    EXPECT_EQ(created.value()->plan().decomposition, edge == 0 ? rt::kDefaultTileEdge : edge);
  }
}

TEST(RaytraceFactory, APathWithoutAKernelIsRefusedAndNamed) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    const auto created = workloads::raytrace::create(config_for(path));
    const bool has_kernel =
        created.has_value() && created.value()->descriptor().paths.contains(path);
    if (has_kernel) {
      continue;
    }
    ASSERT_FALSE(created.has_value()) << simd::describe(path);
    EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::string(simd::describe(path))), std::string::npos)
        << created.error().detail;
  }
  // AVX-512 has no kernel in any build.
  EXPECT_FALSE(workloads::raytrace::create(config_for(simd::IsaPath::kAvx512)).has_value());
}

TEST(RaytraceFactory, ThePathsADescriptorNamesAreTheOnesTheBuildHas) {
  const auto created = workloads::raytrace::create(config_for(simd::IsaPath::kScalar));
  ASSERT_TRUE(created.has_value());
  const simd::IsaSet paths = created.value()->descriptor().paths;
  EXPECT_TRUE(paths.contains(simd::IsaPath::kScalar));
  EXPECT_FALSE(paths.contains(simd::IsaPath::kAvx512));
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(paths, (simd::IsaSet{simd::IsaPath::kScalar}));
#elif defined(__x86_64__)
  EXPECT_EQ(paths, (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2}));
#elif defined(__aarch64__)
  EXPECT_EQ(paths, (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kNeon}));
#endif
  for (const simd::IsaPath path : runnable_paths()) {
    EXPECT_TRUE(workloads::raytrace::create(config_for(path)).has_value()) << simd::describe(path);
  }
}

TEST(RaytraceFactory, TheFootprintOfExactlyOneUnitIsTheSmallestAccepted) {
  workloads::WorkloadConfig config = config_for(simd::IsaPath::kScalar);
  config.footprint_bytes = kUnitBytes;
  const auto one = workloads::raytrace::create(config);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one.value()->plan().units, 1U);

  config.footprint_bytes = kUnitBytes - 1;
  const auto none = workloads::raytrace::create(config);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, workloads::WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(none.error().detail.find(std::to_string(kUnitBytes)), std::string::npos);

  config.footprint_bytes = 5 * kUnitBytes + kUnitBytes - 1;
  EXPECT_EQ(workloads::raytrace::create(config).value()->plan().units, 5U);
  config.footprint_bytes = 0;
  EXPECT_FALSE(workloads::raytrace::create(config).has_value());
}

TEST(RaytraceFactory, TheDescriptorSaysWhatTheSpecificationDeclares) {
  const auto created = workloads::raytrace::create(config_for(simd::IsaPath::kScalar));
  ASSERT_TRUE(created.has_value());
  const workloads::Descriptor& descriptor = created.value()->descriptor();
  EXPECT_EQ(descriptor.name, "raytrace");
  EXPECT_EQ(descriptor.version, 1U);
  EXPECT_EQ(descriptor.determinism, workloads::DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kExact);
}

TEST(RaytraceFactory, ThePlanIsTheSpecificationsGeometry) {
  const auto created = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, 8));
  ASSERT_TRUE(created.has_value());
  const workloads::Plan& plan = created.value()->plan();
  EXPECT_EQ(plan.input_bytes, rt::kInputBytes);
  EXPECT_EQ(plan.output_bytes, rt::kOutputBytes);
  EXPECT_EQ(plan.scratch_bytes, 0U) << "the scene and the hierarchy live on the caller's stack";
  EXPECT_EQ(plan.units, (1U << 20U) / kUnitBytes);
  EXPECT_EQ(plan.decomposition, 8U);
  EXPECT_EQ(plan.logical_bytes, rt::kInputBytes + rt::kOutputBytes + rt::kPixels * 8U);
  EXPECT_EQ(plan.operations, rt::kPixels * rt::kPrimitives);
}

TEST(RaytraceFactory, TheSeedIsTheOnlyConfigurationInputDependsOn) {
  const auto base = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, 8, 5));
  const auto other_edge = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, 32, 5));
  const auto other_seed = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, 8, 6));
  ASSERT_TRUE(base.has_value() && other_edge.has_value() && other_seed.has_value());
  EXPECT_EQ(generate_unit(*base.value(), 4), generate_unit(*other_edge.value(), 4));
  EXPECT_NE(generate_unit(*base.value(), 4), generate_unit(*other_seed.value(), 4));
}

TEST(RaytraceOutputLayout, EveryByteIsWrittenAndEachRegionHoldsWhatTheSpecificationSays) {
  SerialExecutor serial;
  for (const std::uint32_t edge : {8U, 16U, 32U}) {
    const auto workload = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, edge));
    ASSERT_TRUE(workload.has_value());
    const Bytes input = generate_unit(*workload.value(), 2);
    const Bytes zeros = compute_unit(*workload.value(), 2, input, serial, std::byte{0x00});
    const Bytes ones = compute_unit(*workload.value(), 2, input, serial, std::byte{0xFF});
    ASSERT_EQ(zeros, ones) << "a byte left alone would survive in one of the two";
    ASSERT_EQ(zeros.size(), rt::kOutputBytes);

    std::size_t hits = 0;
    for (std::size_t p = 0; p < rt::kPixels; ++p) {
      const std::byte* record = zeros.data() + rt::pixel_offset(p);
      std::uint32_t primitive = 0;
      std::uint32_t padding = 0xFFFFFFFFU;
      std::memcpy(&primitive, record + 8, 4);
      std::memcpy(&padding, record + 12, 4);
      EXPECT_EQ(padding, 0U) << "pixel " << p;
      const double t = double_at(std::span<const std::byte>(record, 8), 0);
      if (primitive == rt::kNoHit) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(t), rt::kNoHitDistanceBits) << "pixel " << p;
      } else {
        ++hits;
        EXPECT_LT(primitive, rt::kPrimitives);
        EXPECT_GE(t, 3.75);
      }
    }
    EXPECT_GT(hits, 0U);
    for (std::size_t k = 0; k < rt::kMaxTiles; ++k) {
      const double sum =
          double_at(std::span<const std::byte>(zeros).subspan(rt::kTileSumsOffset), k);
      if (k < rt::tile_count(edge)) {
        EXPECT_EQ(sum, tile_sum_of(zeros, edge, k)) << "tile " << k;
      } else {
        const auto bits = std::bit_cast<std::uint64_t>(sum);
        EXPECT_EQ(bits, 0U) << "slot " << k << " beyond the tiles is zero bytes";
      }
    }
  }
}

TEST(RaytraceOutputLayout, ATileWithoutAHitSumsToPositiveZero) {
  // A scene with every primitive far from the view: no hit, every sum +0.0.
  SerialExecutor serial;
  const auto workload = workloads::raytrace::create(config_for(simd::IsaPath::kScalar, 8));
  ASSERT_TRUE(workload.has_value());
  Bytes input(rt::kInputBytes);
  for (std::size_t slot = 0; slot < rt::kInputBytes / 8; ++slot) {
    set_double(input, slot, 1.0e6);
  }
  const Bytes output = compute_unit(*workload.value(), 0, input, serial);
  for (std::size_t k = 0; k < rt::kMaxTiles; ++k) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(
                  double_at(std::span<const std::byte>(output).subspan(rt::kTileSumsOffset), k)),
              0U)
        << "tile " << k;
  }
  for (std::size_t p = 0; p < rt::kPixels; ++p) {
    std::uint32_t primitive = 0;
    std::memcpy(&primitive, output.data() + rt::pixel_offset(p) + 8, 4);
    EXPECT_EQ(primitive, rt::kNoHit);
  }
}

}  // namespace
}  // namespace loadforge::tests::raytrace
