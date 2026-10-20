// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a unit's input is drawn and read back: draw e of unit `index` is
// u64_of(block_at(seed, index, e)), decoded by spec.hpp's helpers, in the
// order the specification lays the draws out.

#include "workloads/raytrace/generator.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "oracle/workloads/raytrace/reference.hpp"
#include "primitives/rng/philox.hpp"
#include "workloads/raytrace/scene.hpp"

namespace loadforge::tests::raytrace {
namespace {

Bytes generated(std::uint64_t seed, std::uint64_t index) {
  Bytes input(rt::kInputBytes);
  rt::generate_scene(seed, index, std::span<std::byte, rt::kInputBytes>(input));
  return input;
}

std::uint64_t draw(std::uint64_t seed, std::uint64_t index, std::size_t e) {
  return rng::u64_of(rng::block_at(seed, index, e));
}

TEST(RaytraceGenerator, SpheresAreTheirFourDrawsDecoded) {
  const std::uint64_t seed = 0x1234;
  const Bytes input = generated(seed, 9);
  for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{77}, rt::kSpheres - 1}) {
    const rt::Sphere sphere = sphere_at(input, i);
    EXPECT_EQ(sphere.centre.x, rt::position_xy(draw(seed, 9, 4 * i)));
    EXPECT_EQ(sphere.centre.y, rt::position_xy(draw(seed, 9, 4 * i + 1)));
    EXPECT_EQ(sphere.centre.z, rt::position_z(draw(seed, 9, 4 * i + 2)));
    EXPECT_EQ(sphere.radius, rt::radius_of(draw(seed, 9, 4 * i + 3)));
  }
}

TEST(RaytraceGenerator, TrianglesAreABasePlusThreeOffsetsEachFromItsOwnDraws) {
  const std::uint64_t seed = 0x1234;
  const Bytes input = generated(seed, 9);
  for (const std::size_t j :
       {std::size_t{0}, std::size_t{1}, std::size_t{100}, rt::kTriangles - 1}) {
    const std::size_t first = rt::kSphereDraws + 12 * j;
    const std::array<double, 3> base = {rt::position_xy(draw(seed, 9, first)),
                                        rt::position_xy(draw(seed, 9, first + 1)),
                                        rt::position_z(draw(seed, 9, first + 2))};
    const rt::Triangle triangle = triangle_at(input, j);
    const std::array<const rt::Vec3*, 3> vertices = {&triangle.v0, &triangle.v1, &triangle.v2};
    for (std::size_t k = 0; k < 3; ++k) {
      EXPECT_EQ(vertices.at(k)->x, base[0] + rt::offset_of(draw(seed, 9, first + 3 + 3 * k)));
      EXPECT_EQ(vertices.at(k)->y, base[1] + rt::offset_of(draw(seed, 9, first + 4 + 3 * k)));
      EXPECT_EQ(vertices.at(k)->z, base[2] + rt::offset_of(draw(seed, 9, first + 5 + 3 * k)));
    }
  }
}

TEST(RaytraceGenerator, EveryGeneratedValueIsInsideItsSpecifiedRange) {
  for (std::uint64_t index = 0; index < 25; ++index) {
    const Bytes input = generated(index * 31 + 1, index);
    for (std::size_t i = 0; i < rt::kSpheres; ++i) {
      const rt::Sphere sphere = sphere_at(input, i);
      EXPECT_TRUE(rt::contains(rt::kSphereXy, sphere.centre.x));
      EXPECT_TRUE(rt::contains(rt::kSphereXy, sphere.centre.y));
      EXPECT_TRUE(rt::contains(rt::kSphereZ, sphere.centre.z));
      EXPECT_TRUE(rt::contains(rt::kRadius, sphere.radius));
    }
    for (std::size_t j = 0; j < rt::kTriangles; ++j) {
      const rt::Triangle triangle = triangle_at(input, j);
      for (const rt::Vec3* v : {&triangle.v0, &triangle.v1, &triangle.v2}) {
        EXPECT_TRUE(rt::contains(rt::kVertexXy, v->x));
        EXPECT_TRUE(rt::contains(rt::kVertexXy, v->y));
        EXPECT_TRUE(rt::contains(rt::kVertexZ, v->z));
      }
    }
  }
}

TEST(RaytraceGenerator, ItIsAPureFunctionOfSeedAndUnitAndCoversAllTheBytes) {
  EXPECT_EQ(generated(5, 6), generated(5, 6));
  EXPECT_NE(generated(5, 6), generated(5, 7));
  EXPECT_NE(generated(5, 6), generated(6, 6));
  const Bytes input = generated(5, 6);
  for (std::size_t slot = 0; slot < rt::kInputBytes / 8; ++slot) {
    EXPECT_NE(std::bit_cast<std::uint64_t>(double_at(input, slot)), 0U) << "double " << slot;
  }
}

TEST(RaytraceGenerator, DecodingReadsBackWhatGenerationWrote) {
  const Bytes input = generated(3, 4);
  auto scene = std::make_unique<rt::SceneStorage>();
  rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
  for (std::size_t i = 0; i < rt::kSpheres; ++i) {
    const rt::Sphere expected = sphere_at(input, i);
    const rt::Sphere& got = scene->spheres.at(i);
    EXPECT_EQ(got.centre.x, expected.centre.x);
    EXPECT_EQ(got.centre.y, expected.centre.y);
    EXPECT_EQ(got.centre.z, expected.centre.z);
    EXPECT_EQ(got.radius, expected.radius);
  }
  for (std::size_t j = 0; j < rt::kTriangles; ++j) {
    const rt::Triangle expected = triangle_at(input, j);
    const rt::Triangle& got = scene->triangles.at(j);
    EXPECT_EQ(got.v0.x, expected.v0.x);
    EXPECT_EQ(got.v1.y, expected.v1.y);
    EXPECT_EQ(got.v2.z, expected.v2.z);
    EXPECT_EQ(got.v2.x, expected.v2.x);
    EXPECT_EQ(got.v0.z, expected.v0.z);
    EXPECT_EQ(got.v1.x, expected.v1.x);
  }
}

TEST(RaytraceGenerator, AnyBytesDecodeAndANanStaysANan) {
  Bytes input(rt::kInputBytes, std::byte{0xFF});
  auto scene = std::make_unique<rt::SceneStorage>();
  rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
  EXPECT_TRUE(std::isnan(scene->spheres.front().centre.x));
  EXPECT_TRUE(std::isnan(scene->triangles.back().v2.z));
  set_double(input, 0, std::numeric_limits<double>::infinity());
  rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
  EXPECT_TRUE(std::isinf(scene->spheres.front().centre.x));
}

}  // namespace
}  // namespace loadforge::tests::raytrace
