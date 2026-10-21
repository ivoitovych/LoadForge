// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the ray tracing specification carries: its constants, the
// decoders of the generator's draws at both ends of every range, the camera,
// and the two intersection tests, each branch of which is taken here.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "workloads/raytrace/spec.hpp"

namespace loadforge::workloads::raytrace {
namespace {

constexpr double kTwoToMinus18 = 1.0 / 262144.0;
constexpr double kTwoToMinus21 = 1.0 / 2097152.0;
constexpr double kTwoToMinus23 = 1.0 / 8388608.0;
constexpr std::uint64_t kAllBits = ~std::uint64_t{0};

const Vec3 kHeadOn = {0.0, 0.0, -1.0};
const Ray kStraightDown = {kEye, kHeadOn};

TEST(RaytraceSpecDefinition, TheNameAndTheSizes) {
  EXPECT_EQ(kName, "raytrace");
  EXPECT_EQ(kVersion, 1U);
  EXPECT_EQ(kPixels, 4096U);
  EXPECT_EQ(kPrimitives, 256U);
  EXPECT_EQ(kSphereDraws, 512U);
  EXPECT_EQ(kInputBytes, (128U * 4U + 128U * 9U) * 8U);
  EXPECT_EQ(kInputBytes, 13312U);
  EXPECT_EQ(kPixelBytes, 16U);
  EXPECT_EQ(kTileSumsOffset, 65536U);
  EXPECT_EQ(kOutputBytes, 65536U + 64U * 8U);
  EXPECT_EQ(kSampledPixels, 64U);
  EXPECT_EQ(kCheckPixel, "pixel");
  EXPECT_EQ(kCheckTileSum, "tile sum");
}

TEST(RaytraceSpecDefinition, ASamplePerTileOfTheFinestTilingFitsTheSampleCount) {
  EXPECT_EQ(kSampledPixels, tile_count(8));
  EXPECT_EQ(kMaxTiles, tile_count(8));
}

TEST(RaytraceSpecDefinition, AMissIsInfinityAndItsBitsAreTheDocumentedOnes) {
  EXPECT_TRUE(std::isinf(kNoHitDistance));
  EXPECT_GT(kNoHitDistance, 0.0);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(kNoHitDistance), kNoHitDistanceBits);
  EXPECT_EQ(kNoHit, std::numeric_limits<std::uint32_t>::max());
}

TEST(RaytraceSpecDefinition, TileEdgesAndTheirCounts) {
  for (const std::uint32_t edge : {8U, 16U, 32U}) {
    EXPECT_TRUE(is_tile_edge(edge)) << edge;
  }
  for (const std::uint32_t edge : {0U, 1U, 4U, 12U, 64U}) {
    EXPECT_FALSE(is_tile_edge(edge)) << edge;
  }
  EXPECT_TRUE(is_tile_edge(kDefaultTileEdge));
  EXPECT_EQ(tile_count(8), 64U);
  EXPECT_EQ(tile_count(16), 16U);
  EXPECT_EQ(tile_count(32), 4U);
}

TEST(RaytraceSpecDefinition, OffsetsOfPixelsAndTileSums) {
  EXPECT_EQ(pixel_offset(0), 0U);
  EXPECT_EQ(pixel_offset(1), 16U);
  EXPECT_EQ(pixel_offset(kPixels - 1), kTileSumsOffset - kPixelBytes);
  EXPECT_EQ(tile_sum_offset(0), kTileSumsOffset);
  EXPECT_EQ(tile_sum_offset(kMaxTiles - 1), kOutputBytes - sizeof(double));
}

TEST(RaytraceSpecDefinition, TheLatticeIsTheTopTwentyBitsScaled) {
  EXPECT_EQ(lattice(0), 0.0);
  EXPECT_EQ(lattice(kAllBits), 4.0 - kTwoToMinus18);
  EXPECT_EQ(lattice(std::uint64_t{1} << 44U), kTwoToMinus18);
  EXPECT_EQ(lattice((std::uint64_t{1} << 44U) - 1), 0.0) << "the low 44 bits are discarded";
}

TEST(RaytraceSpecDefinition, PositionsReachBothEndsOfTheirRangesExactly) {
  EXPECT_EQ(position_xy(0), kSphereXy.lo);
  EXPECT_EQ(position_xy(kAllBits), kSphereXy.hi);
  EXPECT_EQ(position_z(0), kSphereZ.lo);
  EXPECT_EQ(position_z(kAllBits), kSphereZ.hi);
  EXPECT_EQ(kSphereXy.lo, -2.0);
  EXPECT_EQ(kSphereXy.hi, 2.0 - kTwoToMinus18);
  EXPECT_EQ(kSphereZ.lo, -4.0);
  EXPECT_EQ(kSphereZ.hi, -kTwoToMinus18);
}

TEST(RaytraceSpecDefinition, RadiiReachBothEndsOfTheirRangeExactly) {
  EXPECT_EQ(radius_of(0), kRadius.lo);
  EXPECT_EQ(radius_of(kAllBits), kRadius.hi);
  EXPECT_EQ(kRadius.lo, 0.0625);
  EXPECT_EQ(kRadius.hi, 0.1875 - kTwoToMinus23);
  EXPECT_LT(kRadius.hi, 0.1875);
}

TEST(RaytraceSpecDefinition, OffsetsReachBothEndsOfTheirRangeExactly) {
  EXPECT_EQ(offset_of(0), -0.25);
  EXPECT_EQ(offset_of(kAllBits), 0.25 - kTwoToMinus21);
  EXPECT_EQ(offset_of(std::uint64_t{1} << 63U), 0.0);
}

TEST(RaytraceSpecDefinition, VertexRangesAreTheBaseRangePlusTheOffsetRangeAndTheSumIsExact) {
  EXPECT_EQ(position_xy(0) + offset_of(0), kVertexXy.lo);
  EXPECT_EQ(position_xy(kAllBits) + offset_of(kAllBits), kVertexXy.hi);
  EXPECT_EQ(position_z(0) + offset_of(0), kVertexZ.lo);
  EXPECT_EQ(position_z(kAllBits) + offset_of(kAllBits), kVertexZ.hi);
  EXPECT_EQ(kVertexXy.lo, -2.25);
  EXPECT_EQ(kVertexXy.hi, 2.25 - kTwoToMinus18 - kTwoToMinus21);
  EXPECT_EQ(kVertexZ.lo, -4.25);
  EXPECT_EQ(kVertexZ.hi, 0.25 - kTwoToMinus18 - kTwoToMinus21);
  // Eye (z = 4) outside every primitive; every primitive below z = 0.25.
  EXPECT_LT(kVertexZ.hi, 0.25);
  EXPECT_LT(kSphereZ.hi + kRadius.hi, 0.25);
}

TEST(RaytraceSpecDefinition, AnIntervalIsClosedAndNoNanIsInIt) {
  const Interval interval = {-1.0, 2.0};
  EXPECT_TRUE(contains(interval, -1.0));
  EXPECT_TRUE(contains(interval, 2.0));
  EXPECT_TRUE(contains(interval, 0.5));
  EXPECT_FALSE(contains(interval, std::nextafter(-1.0, -2.0)));
  EXPECT_FALSE(contains(interval, std::nextafter(2.0, 3.0)));
  EXPECT_FALSE(contains(interval, std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(contains(interval, std::numeric_limits<double>::infinity()));
}

TEST(RaytraceSpecDefinition, ThePixelDirectionIsExactAndIsMirroredAboutTheCentre) {
  const Vec3 top_left = pixel_direction(0, 0);
  EXPECT_EQ(top_left.x, -63.0 / 256.0);
  EXPECT_EQ(top_left.y, 63.0 / 256.0);
  EXPECT_EQ(top_left.z, -1.0);
  const Vec3 bottom_right = pixel_direction(63, 63);
  EXPECT_EQ(bottom_right.x, 63.0 / 256.0);
  EXPECT_EQ(bottom_right.y, -63.0 / 256.0);
  EXPECT_EQ(bottom_right.z, -1.0);
  const Vec3 inner = pixel_direction(32, 31);
  EXPECT_EQ(inner.x, 1.0 / 256.0);
  EXPECT_EQ(inner.y, 1.0 / 256.0);
}

TEST(RaytraceSpecDefinition, ThePixelRayStartsAtTheEyeAndGoesInThePixelDirection) {
  const Ray ray = pixel_ray(5, 9);
  EXPECT_EQ(ray.origin.z, kEye.z);
  EXPECT_EQ(ray.direction.x, pixel_direction(5, 9).x);
  EXPECT_EQ(ray.direction.y, pixel_direction(5, 9).y);
}

TEST(RaytraceSpecDefinition, TheEyeIsAboveTheScene) {
  EXPECT_EQ(kEye.x, 0.0);
  EXPECT_EQ(kEye.y, 0.0);
  EXPECT_EQ(kEye.z, 4.0);
}

TEST(RaytraceSpecDefinition, SubtractionAndTheCrossProductAreComponentWise) {
  const Vec3 diff = sub(Vec3{3.0, 5.0, 7.0}, Vec3{1.0, 2.0, 4.0});
  EXPECT_EQ(diff.x, 2.0);
  EXPECT_EQ(diff.y, 3.0);
  EXPECT_EQ(diff.z, 3.0);
  const Vec3 k = cross(Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0});
  EXPECT_EQ(k.x, 0.0);
  EXPECT_EQ(k.y, 0.0);
  EXPECT_EQ(k.z, 1.0);
  const Vec3 mixed = cross(Vec3{2.0, 3.0, 4.0}, Vec3{5.0, 6.0, 7.0});
  EXPECT_EQ(mixed.x, -3.0);
  EXPECT_EQ(mixed.y, 6.0);
  EXPECT_EQ(mixed.z, -3.0);
}

TEST(RaytraceSpecDefinition, TheDotProductIsSummedLeftToRight) {
  // (1e16 + 1) rounds to 1e16, so left to right gives 0; (1 + (-1e16)) + 1e16 would not.
  EXPECT_EQ(dot(Vec3{1.0, 1.0, 1.0}, Vec3{1e16, 1.0, -1e16}), 0.0);
  EXPECT_EQ(dot(Vec3{1.0, 2.0, 3.0}, Vec3{4.0, 5.0, 6.0}), 32.0);
}

TEST(RaytraceSpecDefinition, ASphereIsHitAtItsNearSurface) {
  EXPECT_EQ(ray_sphere(kStraightDown, Sphere{Vec3{0.0, 0.0, -1.0}, 1.0}), 4.0);
}

TEST(RaytraceSpecDefinition, ATangentRayIsAHit) {
  EXPECT_EQ(ray_sphere(kStraightDown, Sphere{Vec3{1.0, 0.0, -1.0}, 1.0}), 5.0);
}

TEST(RaytraceSpecDefinition, ASphereTheRayPassesIsAMiss) {
  EXPECT_EQ(ray_sphere(kStraightDown, Sphere{Vec3{2.0, 0.0, -1.0}, 1.0}), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ASphereBehindTheOriginIsAMiss) {
  EXPECT_EQ(ray_sphere(kStraightDown, Sphere{Vec3{0.0, 0.0, 10.0}, 1.0}), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ANanRadiusIsAMissNotAHit) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(ray_sphere(kStraightDown, Sphere{Vec3{0.0, 0.0, -1.0}, nan}), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ASphereMatchesAnIndependentComputationOfThePixel) {
  // Recomputed outside C++ (Python floats: binary64, no fused operations), for pixel (40, 30).
  const double t = ray_sphere(pixel_ray(40, 30), Sphere{Vec3{0.375, 0.0625, -1.5}, 0.125});
  EXPECT_EQ(t, 0x1.5815506713b43p+2);
}

TEST(RaytraceSpecDefinition, ATriangleIsHitAtItsPlane) {
  const Triangle triangle = {Vec3{-1.0, -1.0, 0.0}, Vec3{1.0, -1.0, 0.0}, Vec3{0.0, 1.0, 0.0}};
  EXPECT_EQ(ray_triangle(kStraightDown, triangle), 4.0);
}

TEST(RaytraceSpecDefinition, ARayParallelToTheTriangleIsAMiss) {
  const Triangle triangle = {Vec3{-1.0, -1.0, 0.0}, Vec3{1.0, -1.0, 0.0}, Vec3{0.0, 1.0, 0.0}};
  EXPECT_EQ(ray_triangle(Ray{kEye, Vec3{1.0, 0.0, 0.0}}, triangle), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ARayOutsideAnEdgeIsAMissWhicheverEdge) {
  const Triangle triangle = {Vec3{-1.0, -1.0, 0.0}, Vec3{1.0, -1.0, 0.0}, Vec3{0.0, 1.0, 0.0}};
  EXPECT_EQ(ray_triangle(Ray{Vec3{-2.0, 0.0, 4.0}, kHeadOn}, triangle), kNoHitDistance) << "u < 0";
  EXPECT_EQ(ray_triangle(Ray{Vec3{2.0, -1.0, 4.0}, kHeadOn}, triangle), kNoHitDistance) << "u > 1";
  EXPECT_EQ(ray_triangle(Ray{Vec3{0.0, -2.0, 4.0}, kHeadOn}, triangle), kNoHitDistance) << "v < 0";
  EXPECT_EQ(ray_triangle(Ray{Vec3{1.0, 1.0, 4.0}, kHeadOn}, triangle), kNoHitDistance)
      << "u + v > 1";
}

TEST(RaytraceSpecDefinition, ATriangleBehindTheOriginIsAMiss) {
  const Triangle triangle = {Vec3{-1.0, -1.0, 0.0}, Vec3{1.0, -1.0, 0.0}, Vec3{0.0, 1.0, 0.0}};
  EXPECT_EQ(ray_triangle(Ray{Vec3{0.0, 0.0, -4.0}, kHeadOn}, triangle), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ANanVertexIsAMissNotAHit) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Triangle triangle = {Vec3{nan, -1.0, 0.0}, Vec3{1.0, -1.0, 0.0}, Vec3{0.0, 1.0, 0.0}};
  EXPECT_EQ(ray_triangle(kStraightDown, triangle), kNoHitDistance);
}

TEST(RaytraceSpecDefinition, ATriangleMatchesAnIndependentComputationOfThePixel) {
  const Triangle triangle = {Vec3{-1.25, -0.5, -2.0}, Vec3{0.75, -0.75, -1.5},
                             Vec3{0.0, 1.0, -2.5}};
  EXPECT_EQ(ray_triangle(pixel_ray(31, 28), triangle), 0x1.861fd8cdc7a7dp+2);
  EXPECT_EQ(ray_triangle(pixel_ray(28, 24), triangle), 0x1.8e19c798e19c8p+2);
}

}  // namespace
}  // namespace loadforge::workloads::raytrace
