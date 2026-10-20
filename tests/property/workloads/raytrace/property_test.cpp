// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the image that the specification states or implies, held
// over many generated scenes; and metamorphic relations, each a change to the
// input whose effect on the image is known without tracing it by hand.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/raytrace.hpp"

namespace loadforge::tests::raytrace {
namespace {

struct Pixel {
  double t = 0.0;
  std::uint32_t primitive = 0;
  std::uint32_t padding = 0;
};

Pixel pixel_of(const Bytes& output, std::size_t p) {
  Pixel pixel;
  std::memcpy(&pixel.t, output.data() + rt::pixel_offset(p), 8);
  std::memcpy(&pixel.primitive, output.data() + rt::pixel_offset(p) + 8, 4);
  std::memcpy(&pixel.padding, output.data() + rt::pixel_offset(p) + 12, 4);
  return pixel;
}

class Traced {
 public:
  explicit Traced(simd::IsaPath path, std::uint32_t edge = 16, std::uint64_t seed = 31)
      : workload_(std::move(workloads::raytrace::create(config_for(path, edge, seed))).value()) {}

  [[nodiscard]] Bytes input(std::uint64_t unit) const { return generate_unit(*workload_, unit); }
  [[nodiscard]] Bytes image(const Bytes& input) {
    return compute_unit(*workload_, 0, input, executor_);
  }

 private:
  std::unique_ptr<workloads::Workload> workload_;
  SerialExecutor executor_;
};

TEST(RaytraceProperties, AGeneratedSceneHitsBetweenATenthAndNineTenthsOfItsPixels) {
  // spec.hpp: measured about 45%, and the workload's T4 tests hold it to the
  // band.
  for (const simd::IsaPath path : runnable_paths()) {
    Traced traced(path);
    std::size_t total = 0;
    for (std::uint64_t unit = 0; unit < 40; ++unit) {
      const Bytes output = traced.image(traced.input(unit));
      std::size_t hits = 0;
      for (std::size_t p = 0; p < rt::kPixels; ++p) {
        hits += pixel_of(output, p).primitive != rt::kNoHit ? 1 : 0;
      }
      EXPECT_GT(hits, rt::kPixels / 10) << simd::describe(path) << " unit " << unit;
      EXPECT_LT(hits, rt::kPixels * 9 / 10) << simd::describe(path) << " unit " << unit;
      total += hits;
    }
    const double fraction = static_cast<double>(total) / (40.0 * rt::kPixels);
    EXPECT_GT(fraction, 0.3) << "about 45% was measured";
    EXPECT_LT(fraction, 0.6);
  }
}

TEST(RaytraceProperties, EveryRecordIsWellFormedAndEveryHitIsFarFromTheEye) {
  Traced traced(runnable_paths().back());
  for (std::uint64_t unit = 0; unit < 40; ++unit) {
    const Bytes output = traced.image(traced.input(unit));
    for (std::size_t p = 0; p < rt::kPixels; ++p) {
      const Pixel pixel = pixel_of(output, p);
      ASSERT_EQ(pixel.padding, 0U);
      if (pixel.primitive == rt::kNoHit) {
        ASSERT_EQ(std::bit_cast<std::uint64_t>(pixel.t), rt::kNoHitDistanceBits);
      } else {
        ASSERT_LT(pixel.primitive, rt::kPrimitives);
        ASSERT_TRUE(std::isfinite(pixel.t));
        ASSERT_GE(pixel.t, 3.75) << "every primitive lies below z = 0.25 and the eye is at z = 4";
      }
    }
  }
}

TEST(RaytraceProperties, ATileSumIsTheSumOfItsHitsAndNoLessThanTheirCountTimesTheNearestPossible) {
  for (const std::uint32_t edge : {8U, 16U, 32U}) {
    Traced traced(simd::IsaPath::kScalar, edge);
    const Bytes output = traced.image(traced.input(3));
    for (std::size_t tile = 0; tile < rt::tile_count(edge); ++tile) {
      std::size_t hits = 0;
      const std::size_t per_row = rt::kWidth / edge;
      for (std::size_t y = (tile / per_row) * edge; y < (tile / per_row + 1) * edge; ++y) {
        for (std::size_t x = (tile % per_row) * edge; x < (tile % per_row + 1) * edge; ++x) {
          hits += pixel_of(output, y * rt::kWidth + x).primitive != rt::kNoHit ? 1 : 0;
        }
      }
      const double sum =
          double_at(std::span<const std::byte>(output).subspan(rt::kTileSumsOffset), tile);
      if (hits == 0) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(sum), 0U) << "tile " << tile << " edge " << edge;
      } else {
        EXPECT_GE(sum, 3.75 * static_cast<double>(hits)) << "tile " << tile << " edge " << edge;
      }
    }
  }
}

TEST(RaytraceProperties, TakingAPrimitiveOutOfSightChangesOnlyThePixelsItOwned) {
  for (const simd::IsaPath path : runnable_paths()) {
    Traced traced(path);
    const Bytes input = traced.input(5);
    const Bytes before = traced.image(input);
    for (const std::size_t victim :
         {std::size_t{0}, std::size_t{57}, std::size_t{128}, std::size_t{200}, std::size_t{255}}) {
      Bytes changed = input;
      if (victim < rt::kSpheres) {
        set_double(changed, victim * 4, 1.0e6);
      } else {
        for (std::size_t c = 0; c < 9; ++c) {
          set_double(changed, rt::kSpheres * 4 + (victim - rt::kSpheres) * 9 + c,
                     1.0e6 + static_cast<double>(c));
        }
      }
      const Bytes after = traced.image(changed);
      for (std::size_t p = 0; p < rt::kPixels; ++p) {
        if (pixel_of(before, p).primitive == victim) {
          EXPECT_NE(pixel_of(after, p).primitive, victim);
        } else {
          EXPECT_EQ(std::memcmp(before.data() + rt::pixel_offset(p),
                                after.data() + rt::pixel_offset(p), 16),
                    0)
              << simd::describe(path) << " victim " << victim << " pixel " << p;
        }
      }
    }
  }
}

TEST(RaytraceProperties, PermutingTheSpheresPermutesTheirIndicesAndNothingElse) {
  for (const simd::IsaPath path : runnable_paths()) {
    Traced traced(path);
    const Bytes input = traced.input(6);
    const Bytes before = traced.image(input);
    std::array<std::size_t, rt::kSpheres> sigma{};
    std::iota(sigma.begin(), sigma.end(), std::size_t{0});
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    for (std::size_t i = rt::kSpheres - 1; i > 0; --i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      std::swap(sigma.at(i), sigma.at((state >> 33U) % (i + 1)));
    }
    Bytes permuted = input;
    for (std::size_t i = 0; i < rt::kSpheres; ++i) {
      std::memcpy(permuted.data() + sigma.at(i) * 32, input.data() + i * 32, 32);
    }
    const Bytes after = traced.image(permuted);
    for (std::size_t p = 0; p < rt::kPixels; ++p) {
      const Pixel was = pixel_of(before, p);
      const Pixel now = pixel_of(after, p);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(was.t), std::bit_cast<std::uint64_t>(now.t))
          << simd::describe(path) << " pixel " << p;
      const std::uint32_t expected = was.primitive < rt::kSpheres
                                         ? static_cast<std::uint32_t>(sigma.at(was.primitive))
                                         : was.primitive;
      EXPECT_EQ(now.primitive, expected) << simd::describe(path) << " pixel " << p;
    }
  }
}

TEST(RaytraceProperties, ACopyOfAPrimitiveAtAHigherIndexNeverWinsATie) {
  for (const simd::IsaPath path : runnable_paths()) {
    Traced traced(path);
    const Bytes input = traced.input(7);
    const Bytes before = traced.image(input);
    // Sphere 100 becomes a copy of sphere 5; triangle 90 a copy of triangle 3.
    Bytes copied = input;
    constexpr std::size_t kSphereBytes = 32;
    constexpr std::size_t kTriangleBytes = 72;
    std::memcpy(copied.data() + std::size_t{100} * kSphereBytes,
                input.data() + std::size_t{5} * kSphereBytes, kSphereBytes);
    const std::size_t triangles = rt::kSpheres * kSphereBytes;
    std::memcpy(copied.data() + triangles + std::size_t{90} * kTriangleBytes,
                input.data() + triangles + std::size_t{3} * kTriangleBytes, kTriangleBytes);
    const Bytes after = traced.image(copied);
    for (std::size_t p = 0; p < rt::kPixels; ++p) {
      const Pixel now = pixel_of(after, p);
      EXPECT_NE(now.primitive, 100U) << simd::describe(path) << " pixel " << p;
      EXPECT_NE(now.primitive, rt::kSpheres + 90U) << simd::describe(path) << " pixel " << p;
      const Pixel was = pixel_of(before, p);
      if (was.primitive != 100U && was.primitive != rt::kSpheres + 90U) {
        EXPECT_EQ(std::memcmp(before.data() + rt::pixel_offset(p),
                              after.data() + rt::pixel_offset(p), 16),
                  0)
            << simd::describe(path) << " pixel " << p;
      }
    }
  }
}

TEST(RaytraceProperties, ScenesFromEverySeedAndFromTheHighestUnitIndicesAreTraced) {
  SerialExecutor serial;
  for (const std::uint64_t seed : {0ULL, 1ULL, 0xFFFFFFFFULL, 0x100000000ULL, ~0ULL}) {
    for (const std::uint64_t unit : {0ULL, 0xFFFFFFFFULL, 0x100000000ULL, ~0ULL}) {
      for (const simd::IsaPath path : runnable_paths()) {
        const auto workload =
            std::move(workloads::raytrace::create(config_for(path, 16, seed))).value();
        const Bytes input = generate_unit(*workload, unit);
        const Bytes output = compute_unit(*workload, unit, input, serial);
        EXPECT_EQ(compare_with_brute_force(input, output, 16).verdict, Verdict::kAgreement)
            << simd::describe(path) << " seed " << seed << " unit " << unit;
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::tests::raytrace
