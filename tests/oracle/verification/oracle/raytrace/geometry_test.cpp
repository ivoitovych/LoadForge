// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 of the ray tracing oracle: the specification's arithmetic, and so the
// oracle built on it, against geometry done another way. There is no
// reference tool for a synthetic scene, so the reference is mathematics:
//
//   * every pixel of generated scenes (the specification's recipe, through
//     primitives/rng) solved again in long double -- a sphere from the
//     quadratic in t, a triangle from the plane's t and the signs of the
//     three edge functions -- with the hit tested a little tighter and a
//     little looser than exact; where the two disagree the pixel is on an
//     edge and not judged, and everywhere else the oracle's record must name
//     the same primitive at the same distance;
//   * scenes whose answer is known by hand: a triangle in the plane z = 0 is
//     met at t = 4 exactly where the ray's x and y fall inside it, and a
//     sphere on the axis is met at the distance the quadratic gives.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "unit/verification/oracle/raytrace/scene_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/raytrace/oracle.hpp"
#include "workloads/raytrace/spec.hpp"

namespace loadforge::verification::oracle::raytrace {
namespace {

namespace spec = workloads::raytrace;
namespace rt = test::raytrace;

using Ld = long double;

struct Ld3 {
  Ld x;
  Ld y;
  Ld z;
};

Ld3 ld(const spec::Vec3& v) { return {v.x, v.y, v.z}; }
Ld3 minus(const Ld3& a, const Ld3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Ld dotp(const Ld3& a, const Ld3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Ld3 crossp(const Ld3& a, const Ld3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct Hit {
  Ld t = -1;
  std::size_t primitive = spec::kNoHit;
};

/// The nearest primitive for the pixel, with every hit test loosened by `slack`
/// (positive: more generous; negative: stricter).
Hit nearest(const rt::Values& values, std::size_t p, Ld slack) {
  const spec::Ray ray = spec::pixel_ray(static_cast<std::uint32_t>(p % spec::kWidth),
                                        static_cast<std::uint32_t>(p / spec::kWidth));
  const Ld3 o = ld(ray.origin);
  const Ld3 d = ld(ray.direction);
  Hit best;
  for (std::size_t i = 0; i < spec::kSpheres; ++i) {
    const spec::Sphere sphere = rt::sphere_of(values, i);
    const Ld3 oc = minus(o, ld(sphere.centre));
    const Ld a = dotp(d, d);
    const Ld b = 2 * dotp(d, oc);
    const Ld r = sphere.radius;
    const Ld c = dotp(oc, oc) - r * r;
    const Ld disc = b * b - 4 * a * c;
    // Looser or tighter by a relative amount of the discriminant's scale.
    if (disc + slack * b * b < 0) {
      continue;
    }
    const Ld t = (-b - std::sqrt(std::fmax(disc, Ld{0}))) / (2 * a);
    if (t > 0 && (best.primitive == spec::kNoHit || t < best.t)) {
      best = Hit{t, i};
    }
  }
  for (std::size_t j = 0; j < spec::kTriangles; ++j) {
    const spec::Triangle triangle = rt::triangle_of(values, j);
    const Ld3 v0 = ld(triangle.v0);
    const Ld3 v1 = ld(triangle.v1);
    const Ld3 v2 = ld(triangle.v2);
    const Ld3 n = crossp(minus(v1, v0), minus(v2, v0));
    const Ld denom = dotp(d, n);
    if (denom == 0) {
      continue;
    }
    const Ld t = dotp(minus(v0, o), n) / denom;
    if (!(t > 0)) {
      continue;
    }
    const Ld3 point = {o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
    const Ld n2 = dotp(n, n);
    const std::array<std::array<Ld3, 2>, 3> edges = {
        std::array<Ld3, 2>{v0, v1}, std::array<Ld3, 2>{v1, v2}, std::array<Ld3, 2>{v2, v0}};
    bool inside = true;
    for (const auto& edge : edges) {
      const Ld side = dotp(crossp(minus(edge[1], edge[0]), minus(point, edge[0])), n) / n2;
      if (side < -slack) {
        inside = false;
      }
    }
    if (inside && (best.primitive == spec::kNoHit || t < best.t)) {
      best = Hit{t, spec::kSpheres + j};
    }
  }
  return best;
}

Verdict judge(std::uint64_t index, const rt::Values& scene, const rt::Bytes& output) {
  const RaytraceOracle oracle(spec::kDefaultTileEdge);
  const rt::Bytes input = rt::bytes_of(scene);
  return oracle.check(UnitEvidence{index, input, output});
}

TEST(RaytraceGeometry, GeneratedScenesAgreeWithAnExtendedPrecisionSolutionPixelByPixel) {
  std::size_t judged = 0;
  std::size_t skipped = 0;
  std::size_t hits = 0;
  for (std::uint64_t index = 0; index < 6; ++index) {
    const rt::Values scene = rt::generated(31, index);
    const rt::Bytes output = rt::render(scene);
    for (std::size_t p = 0; p < spec::kPixels; ++p) {
      const Hit tight = nearest(scene, p, -1e-9L);
      const Hit loose = nearest(scene, p, 1e-9L);
      if (tight.primitive != loose.primitive) {
        ++skipped;
        continue;
      }
      ++judged;
      const std::uint32_t stored = rt::get32(output, spec::pixel_offset(p) + 8);
      const auto t = std::bit_cast<double>(rt::get64(output, spec::pixel_offset(p)));
      if (tight.primitive == spec::kNoHit) {
        EXPECT_EQ(stored, spec::kNoHit) << "unit " << index << " pixel " << p;
        EXPECT_EQ(t, spec::kNoHitDistance);
      } else {
        ++hits;
        EXPECT_EQ(stored, tight.primitive) << "unit " << index << " pixel " << p;
        EXPECT_NEAR(static_cast<double>(tight.t) / t, 1.0, 1e-9)
            << "unit " << index << " pixel " << p;
      }
    }
    EXPECT_TRUE(std::holds_alternative<Agreement>(judge(index, scene, output))) << "unit " << index;
  }
  EXPECT_LT(skipped, judged / 50) << "pixels on an edge should be rare";
  EXPECT_GT(hits, judged / 5);
}

TEST(RaytraceGeometry, ATriangleInThePlaneOfTheOriginIsMetAtFourWhereTheRayFallsInsideIt) {
  rt::Values scene = rt::hidden();
  rt::set_triangle(scene, 0,
                   spec::Triangle{spec::Vec3{-1.0, -0.75, 0.0}, spec::Vec3{1.25, -0.5, 0.0},
                                  spec::Vec3{0.0, 1.0, 0.0}});
  const rt::Bytes output = rt::render(scene);
  const spec::Triangle triangle = rt::triangle_of(scene, 0);
  std::size_t inside = 0;
  for (std::size_t p = 0; p < spec::kPixels; ++p) {
    const spec::Vec3 d = spec::pixel_direction(static_cast<std::uint32_t>(p % spec::kWidth),
                                               static_cast<std::uint32_t>(p / spec::kWidth));
    // The ray meets z = 0 after t = 4, at (4 dx, 4 dy): all exact.
    const double x = 4.0 * d.x;
    const double y = 4.0 * d.y;
    const auto side = [&](const spec::Vec3& a, const spec::Vec3& b) {
      return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    const double s0 = side(triangle.v0, triangle.v1);
    const double s1 = side(triangle.v1, triangle.v2);
    const double s2 = side(triangle.v2, triangle.v0);
    if (s0 * s1 * s2 == 0.0) {
      continue;  // on an edge's line: the hand answer is ambiguous there
    }
    const bool by_hand = (s0 > 0 && s1 > 0 && s2 > 0) || (s0 < 0 && s1 < 0 && s2 < 0);
    const std::uint32_t stored = rt::get32(output, spec::pixel_offset(p) + 8);
    const auto t = std::bit_cast<double>(rt::get64(output, spec::pixel_offset(p)));
    if (by_hand) {
      ++inside;
      EXPECT_EQ(stored, spec::kSpheres) << "pixel " << p;
      EXPECT_NEAR(t, 4.0, 1e-13) << "pixel " << p;
    } else {
      EXPECT_EQ(stored, spec::kNoHit) << "pixel " << p;
    }
  }
  EXPECT_GT(inside, 500U);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(0, scene, output)));
}

TEST(RaytraceGeometry, ASphereIsMetAtTheNearerRootOfTheQuadratic) {
  rt::Values scene = rt::hidden();
  const spec::Sphere sphere{spec::Vec3{0.0, 0.0, -1.0}, 0.125};
  rt::set_sphere(scene, 3, sphere);
  const rt::Bytes output = rt::render(scene);
  std::size_t met = 0;
  for (std::size_t p = 0; p < spec::kPixels; ++p) {
    const spec::Vec3 d = spec::pixel_direction(static_cast<std::uint32_t>(p % spec::kWidth),
                                               static_cast<std::uint32_t>(p / spec::kWidth));
    // |(0, 0, 5) + t d|^2 along the eye's offset from the centre, in long double.
    const Ld a = static_cast<Ld>(d.x) * d.x + static_cast<Ld>(d.y) * d.y + 1;
    const Ld b = 2 * (5.0L * d.z);
    const Ld c = 25.0L - 0.125L * 0.125L;
    const Ld disc = b * b - 4 * a * c;
    const std::uint32_t stored = rt::get32(output, spec::pixel_offset(p) + 8);
    const auto t = std::bit_cast<double>(rt::get64(output, spec::pixel_offset(p)));
    if (disc > 1e-9L) {
      ++met;
      EXPECT_EQ(stored, 3U) << "pixel " << p;
      EXPECT_NEAR(static_cast<double>((-b - std::sqrt(disc)) / (2 * a)) / t, 1.0, 1e-13);
    } else if (disc < -1e-9L) {
      EXPECT_EQ(stored, spec::kNoHit) << "pixel " << p;
    }
  }
  EXPECT_GT(met, 20U);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(0, scene, output)));
}

}  // namespace
}  // namespace loadforge::verification::oracle::raytrace

// The contract header registers two suites; this binary runs neither (the
// oracle's own suites do, in tests/unit).
namespace loadforge::contracts {
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
