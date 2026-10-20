// SPDX-License-Identifier: GPL-3.0-or-later
//
// The row kernels: the scalar one and the wide one trace every pixel the same
// way, which is brute force's way, on generated scenes and on scenes made to
// reach each branch of the intersection arithmetic and the tie rule.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/generator.hpp"
#include "workloads/raytrace/kernels.hpp"
#include "workloads/raytrace/packet.hpp"
#include "workloads/raytrace/scene.hpp"

namespace loadforge::tests::raytrace {
namespace {

constexpr double kFar = 1.0e6;

/// An input whose every primitive is out of sight: spheres and triangles at a
/// million units, which no ray of the camera comes near.
Bytes parked_input() {
  Bytes input(rt::kInputBytes);
  for (std::size_t i = 0; i < rt::kSpheres; ++i) {
    set_double(input, i * 4, kFar);
    set_double(input, i * 4 + 1, kFar);
    set_double(input, i * 4 + 2, -kFar);
    set_double(input, i * 4 + 3, 0.1);
  }
  for (std::size_t j = 0; j < rt::kTriangles; ++j) {
    const std::size_t first = rt::kSpheres * 4 + j * 9;
    const std::array<double, 9> far = {kFar, kFar, -kFar,        kFar + 1, kFar, -kFar,
                                       kFar,        kFar + 1, -kFar};
    for (std::size_t c = 0; c < 9; ++c) {
      set_double(input, first + c, far.at(c));
    }
  }
  return input;
}

void put_sphere(Bytes& input, std::size_t i, double x, double y, double z, double r) {
  set_double(input, i * 4, x);
  set_double(input, i * 4 + 1, y);
  set_double(input, i * 4 + 2, z);
  set_double(input, i * 4 + 3, r);
}

void put_triangle(Bytes& input, std::size_t j, const std::array<double, 9>& v) {
  for (std::size_t c = 0; c < 9; ++c) {
    set_double(input, rt::kSpheres * 4 + j * 9 + c, v.at(c));
  }
}

/// The pixel records a kernel writes for `input`.
Bytes records_of(workloads::raytrace::RowKernel kernel, const Bytes& input,
                 double margin = rt::kBoundMargin) {
  auto scene = std::make_unique<rt::SceneStorage>();
  rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
  rt::build_bvh(*scene, margin);
  Bytes records(rt::kTileSumsOffset, std::byte{0xAB});
  for (std::uint32_t y = 0; y < rt::kHeight; ++y) {
    kernel(*scene, y,
           std::span<std::byte, rt::kRowBytes>(records.data() + y * rt::kRowBytes, rt::kRowBytes));
  }
  return records;
}

Bytes brute_force_records(const Bytes& input) {
  Bytes out(rt::kOutputBytes);
  for (std::uint32_t y = 0; y < rt::kHeight; ++y) {
    for (std::uint32_t x = 0; x < rt::kWidth; ++x) {
      put_record(out, y * rt::kWidth + x, brute_force_pixel(input, x, y));
    }
  }
  out.resize(rt::kTileSumsOffset);
  return out;
}

std::vector<std::pair<std::string, workloads::raytrace::RowKernel>> runnable_kernels() {
  const auto table = workloads::raytrace::row_kernels();
  std::vector<std::pair<std::string, workloads::raytrace::RowKernel>> kernels;
  for (const simd::IsaPath path : runnable_paths()) {
    kernels.emplace_back(std::string(simd::describe(path)), simd::kernel_at(table, path));
  }
  return kernels;
}

void expect_every_kernel_matches_brute_force(const Bytes& input, const char* what) {
  const Bytes expected = brute_force_records(input);
  for (const auto& [name, kernel] : runnable_kernels()) {
    EXPECT_EQ(records_of(kernel, input), expected) << name << ": " << what;
  }
}

std::uint32_t primitive_at(const Bytes& records, std::uint32_t x, std::uint32_t y) {
  std::uint32_t primitive = 0;
  std::memcpy(&primitive, records.data() + rt::pixel_offset(y * rt::kWidth + x) + 8, 4);
  return primitive;
}

double t_at(const Bytes& records, std::uint32_t x, std::uint32_t y) {
  return double_at(std::span<const std::byte>(records).subspan(rt::pixel_offset(y * rt::kWidth + x)),
                   0);
}

TEST(RaytraceKernels, TheTableHoldsTheScalarKernelAndTheArchitecturesWideOne) {
  const auto table = workloads::raytrace::row_kernels();
  EXPECT_NE(table.scalar, nullptr);
  EXPECT_EQ(table.avx512, nullptr);
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(table.avx2, nullptr);
  EXPECT_EQ(table.neon, nullptr);
#elif defined(__x86_64__)
  EXPECT_NE(table.avx2, nullptr);
  EXPECT_EQ(table.neon, nullptr);
#elif defined(__aarch64__)
  EXPECT_EQ(table.avx2, nullptr);
  EXPECT_NE(table.neon, nullptr);
#endif
}

TEST(RaytraceKernels, GeneratedScenesMatchBruteForce) {
  for (std::uint64_t unit = 0; unit < 4; ++unit) {
    Bytes input(rt::kInputBytes);
    rt::generate_scene(unit * 17 + 3, unit, std::span<std::byte, rt::kInputBytes>(input));
    expect_every_kernel_matches_brute_force(input, "generated");
  }
}

TEST(RaytraceKernels, AnEmptyViewIsAllMisses) {
  const Bytes input = parked_input();
  for (const auto& [name, kernel] : runnable_kernels()) {
    const Bytes records = records_of(kernel, input);
    for (std::uint32_t p = 0; p < rt::kPixels; ++p) {
      ASSERT_EQ(primitive_at(records, p % rt::kWidth, p / rt::kWidth), rt::kNoHit) << name;
    }
    EXPECT_EQ(t_at(records, 0, 0), rt::kNoHitDistance);
  }
}

TEST(RaytraceKernels, ASphereInFrontOfTheCameraIsHitWhereItsDiscIs) {
  Bytes input = parked_input();
  put_sphere(input, 9, 0.0, 0.0, -2.0, 1.0);
  expect_every_kernel_matches_brute_force(input, "one sphere");
  for (const auto& [name, kernel] : runnable_kernels()) {
    const Bytes records = records_of(kernel, input);
    EXPECT_EQ(primitive_at(records, 32, 32), 9U) << name;
    EXPECT_NEAR(t_at(records, 32, 32), 5.0, 0.01) << name;
    EXPECT_EQ(primitive_at(records, 0, 0), rt::kNoHit) << name;
  }
}

TEST(RaytraceKernels, ATriangleIsHitInsideItsEdgesAndNotOutside) {
  Bytes input = parked_input();
  put_triangle(input, 5, {-1.0, -1.0, -1.0, 1.0, -1.0, -1.0, 0.0, 1.0, -1.0});
  expect_every_kernel_matches_brute_force(input, "one triangle");
  for (const auto& [name, kernel] : runnable_kernels()) {
    const Bytes records = records_of(kernel, input);
    EXPECT_EQ(primitive_at(records, 32, 32), rt::kSpheres + 5) << name;
    EXPECT_NEAR(t_at(records, 32, 32), 5.0, 0.01) << name;
    EXPECT_EQ(primitive_at(records, 0, 63), rt::kNoHit) << name;
  }
}

TEST(RaytraceKernels, AnEqualDistanceGoesToTheLowerIndexWhicheverOrderTheyAreVisitedIn) {
  Bytes input = parked_input();
  // Identical spheres at indices 3 and 90, identical triangles at 7 and 120:
  // the same t from each pair, in every pixel they cover.
  for (const std::size_t i : {std::size_t{3}, std::size_t{90}}) {
    put_sphere(input, i, -0.5, 0.0, -2.0, 0.4);
  }
  for (const std::size_t j : {std::size_t{7}, std::size_t{120}}) {
    put_triangle(input, j, {0.3, -0.5, -1.0, 1.3, -0.5, -1.0, 0.8, 0.5, -1.0});
  }
  expect_every_kernel_matches_brute_force(input, "equal distances");
  for (const auto& [name, kernel] : runnable_kernels()) {
    const Bytes records = records_of(kernel, input);
    std::size_t sphere_pixels = 0;
    std::size_t triangle_pixels = 0;
    for (std::uint32_t p = 0; p < rt::kPixels; ++p) {
      const std::uint32_t id = primitive_at(records, p % rt::kWidth, p / rt::kWidth);
      ASSERT_NE(id, 90U) << name;
      ASSERT_NE(id, rt::kSpheres + 120U) << name;
      sphere_pixels += id == 3U ? 1 : 0;
      triangle_pixels += id == rt::kSpheres + 7U ? 1 : 0;
    }
    EXPECT_GT(sphere_pixels, 100U) << name;
    EXPECT_GT(triangle_pixels, 100U) << name;
  }
}

TEST(RaytraceKernels, ANearerPrimitiveOfAHigherIndexBeatsAFartherOfALowerOne) {
  Bytes input = parked_input();
  put_sphere(input, 2, 0.0, 0.0, -3.0, 0.5);
  put_sphere(input, 100, 0.0, 0.0, -1.0, 0.5);
  expect_every_kernel_matches_brute_force(input, "near behind far");
  for (const auto& [name, kernel] : runnable_kernels()) {
    EXPECT_EQ(primitive_at(records_of(kernel, input), 32, 32), 100U) << name;
  }
  // And the other way round: the nearer one of the lower index.
  Bytes swapped = parked_input();
  put_sphere(swapped, 2, 0.0, 0.0, -1.0, 0.5);
  put_sphere(swapped, 100, 0.0, 0.0, -3.0, 0.5);
  expect_every_kernel_matches_brute_force(swapped, "far behind near");
  for (const auto& [name, kernel] : runnable_kernels()) {
    EXPECT_EQ(primitive_at(records_of(kernel, swapped), 32, 32), 2U) << name;
  }
}

TEST(RaytraceKernels, PrimitivesThatCannotBeHitAreMisses) {
  Bytes input = parked_input();
  // The eye inside a sphere: the near root is behind it, and the far root is
  // not tried.
  put_sphere(input, 0, 0.0, 0.0, 4.0, 1.0);
  // A sphere behind the eye.
  put_sphere(input, 1, 0.0, 0.0, 9.0, 1.0);
  // A negative radius is a sphere of its magnitude.
  put_sphere(input, 2, 0.0, 0.0, -2.0, -0.5);
  // A triangle behind the eye; a degenerate one (collinear vertices); one
  // edge-on to the camera.
  put_triangle(input, 0, {-1.0, -1.0, 9.0, 1.0, -1.0, 9.0, 0.0, 1.0, 9.0});
  put_triangle(input, 1, {0.0, 0.0, -1.0, 1.0, 1.0, -1.0, 2.0, 2.0, -1.0});
  put_triangle(input, 2, {-1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, -2.0});
  expect_every_kernel_matches_brute_force(input, "misses");
  for (const auto& [name, kernel] : runnable_kernels()) {
    const Bytes records = records_of(kernel, input);
    EXPECT_EQ(primitive_at(records, 32, 32), 2U) << name << ": only the negative radius is a hit";
  }
}

TEST(RaytraceKernels, NumbersThatAreNotNumbersAreMissesAndMatchBruteForce) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  Bytes input = parked_input();
  put_sphere(input, 0, nan, 0.0, -2.0, 0.5);
  put_sphere(input, 1, 0.0, 0.0, -2.0, nan);
  put_sphere(input, 2, inf, 0.0, -2.0, 0.5);
  put_sphere(input, 3, 0.0, 0.0, -2.0, inf);
  put_sphere(input, 4, 0.2, 0.0, -2.0, 0.3);
  put_triangle(input, 0, {nan, -1.0, -1.0, 1.0, -1.0, -1.0, 0.0, 1.0, -1.0});
  put_triangle(input, 1, {-inf, -1.0, -1.0, inf, -1.0, -1.0, 0.0, 1.0, -1.0});
  expect_every_kernel_matches_brute_force(input, "not numbers");
}

TEST(RaytraceKernels, AllBytesOfAnyKindProduceTheSameRecordsOnEveryPath) {
  for (const std::uint8_t fill : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x7F},
                                  std::uint8_t{0x80}, std::uint8_t{0x3F}}) {
    const Bytes input(rt::kInputBytes, static_cast<std::byte>(fill));
    expect_every_kernel_matches_brute_force(input, "constant bytes");
  }
}

TEST(RaytraceKernels, AShrunkenHierarchyCullsWhatBruteForceFinds) {
  // The falsifiability of every comparison above: boxes made smaller than the
  // primitives they hold are a hierarchy that skips what it should have tried,
  // and the kernels then disagree with brute force.
  Bytes input(rt::kInputBytes);
  rt::generate_scene(2, 2, std::span<std::byte, rt::kInputBytes>(input));
  const Bytes expected = brute_force_records(input);
  for (const auto& [name, kernel] : runnable_kernels()) {
    EXPECT_EQ(records_of(kernel, input), expected) << name;
    EXPECT_NE(records_of(kernel, input, -0.05), expected) << name << ": boxes 0.05 too small";
  }
}

TEST(RaytraceReplaces, ACandidateReplacesOnlyAHitThatIsNearerOrEqualAndLowerIndexed) {
  const double inf = rt::kNoHitDistance;
  using workloads::raytrace::replaces;
  EXPECT_FALSE(replaces(inf, 3, inf, rt::kNoHit)) << "a miss never replaces";
  EXPECT_FALSE(replaces(inf, 3, 5.0, 9)) << "a miss never replaces a hit";
  EXPECT_TRUE(replaces(5.0, 3, inf, rt::kNoHit)) << "any hit beats nothing";
  EXPECT_TRUE(replaces(4.0, 9, 5.0, 3)) << "nearer wins whatever the index";
  EXPECT_FALSE(replaces(6.0, 1, 5.0, 3)) << "farther loses whatever the index";
  EXPECT_TRUE(replaces(5.0, 2, 5.0, 3)) << "equal t, lower index";
  EXPECT_FALSE(replaces(5.0, 4, 5.0, 3)) << "equal t, higher index";
  EXPECT_FALSE(replaces(5.0, 3, 5.0, 3)) << "the same primitive again";
}

}  // namespace
}  // namespace loadforge::tests::raytrace
