// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_RAYTRACE_SCENE_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_RAYTRACE_SCENE_SUPPORT_HPP

// What the ray tracing oracle's tests share, in every tier: scenes (generated
// as workloads/raytrace/spec.hpp says, or built by hand), the units' input
// bytes, and a MODEL of the output. The model is built differently from the
// oracle on purpose: primitive-major, with a running nearest per pixel, where
// the oracle goes pixel by pixel over the primitives. Both take the arithmetic
// from the specification's helpers, as the specification requires, and the
// model never calls the oracle.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "contracts/verification/oracle_case.hpp"
#include "primitives/rng/philox.hpp"
#include "workloads/raytrace/spec.hpp"

namespace loadforge::test::raytrace {

namespace spec = workloads::raytrace;

using Bytes = std::vector<std::byte>;

/// The scene as the doubles of the input, in the input's order.
using Values = std::array<double, spec::kInputBytes / sizeof(double)>;

inline constexpr std::size_t kSphereValues = spec::kSpheres * spec::kSphereDoubles;

inline Bytes bytes_of(const Values& values) {
  Bytes bytes(spec::kInputBytes);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto bits = std::bit_cast<std::uint64_t>(values[i]);
    for (std::size_t k = 0; k < 8; ++k) {
      bytes[i * 8 + k] = static_cast<std::byte>((bits >> (8U * k)) & 0xFFU);
    }
  }
  return bytes;
}

inline Values values_of(const Bytes& bytes) {
  Values values{};
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::uint64_t bits = 0;
    for (std::size_t k = 0; k < 8; ++k) {
      bits |= std::to_integer<std::uint64_t>(bytes[i * 8 + k]) << (8U * k);
    }
    values[i] = std::bit_cast<double>(bits);
  }
  return values;
}

/// The scene unit `index` of run `seed` has, by the specification's recipe.
inline Values generated(std::uint64_t seed, std::uint64_t index) {
  auto draw = [&](std::size_t e) { return rng::u64_of(rng::block_at(seed, index, e)); };
  Values values{};
  for (std::size_t i = 0; i < spec::kSpheres; ++i) {
    values[4 * i] = spec::position_xy(draw(4 * i));
    values[4 * i + 1] = spec::position_xy(draw(4 * i + 1));
    values[4 * i + 2] = spec::position_z(draw(4 * i + 2));
    values[4 * i + 3] = spec::radius_of(draw(4 * i + 3));
  }
  for (std::size_t j = 0; j < spec::kTriangles; ++j) {
    const std::size_t first = spec::kSphereDraws + spec::kTriangleDraws * j;
    const double base[3] = {spec::position_xy(draw(first)), spec::position_xy(draw(first + 1)),
                            spec::position_z(draw(first + 2))};
    for (std::size_t k = 0; k < 3; ++k) {
      for (std::size_t c = 0; c < 3; ++c) {
        values[kSphereValues + 9 * j + 3 * k + c] =
            base[c] + spec::offset_of(draw(first + 3 + 3 * k + c));
      }
    }
  }
  return values;
}

/// A scene no ray meets: every sphere and every triangle at one spot the
/// camera's rays pass by (all in range; the triangles are points).
inline Values hidden() {
  Values values{};
  for (std::size_t i = 0; i < spec::kSpheres; ++i) {
    values[4 * i] = 1.9;
    values[4 * i + 1] = 1.9;
    values[4 * i + 2] = -0.1;
    values[4 * i + 3] = 0.0625;
  }
  for (std::size_t j = 0; j < spec::kTriangles; ++j) {
    for (std::size_t k = 0; k < 3; ++k) {
      values[kSphereValues + 9 * j + 3 * k] = 1.9;
      values[kSphereValues + 9 * j + 3 * k + 1] = 1.9;
      values[kSphereValues + 9 * j + 3 * k + 2] = -0.1;
    }
  }
  return values;
}

inline void set_sphere(Values& values, std::size_t i, const spec::Sphere& sphere) {
  values[4 * i] = sphere.centre.x;
  values[4 * i + 1] = sphere.centre.y;
  values[4 * i + 2] = sphere.centre.z;
  values[4 * i + 3] = sphere.radius;
}

inline void set_triangle(Values& values, std::size_t j, const spec::Triangle& triangle) {
  const std::array<spec::Vec3, 3> vertices = {triangle.v0, triangle.v1, triangle.v2};
  for (std::size_t k = 0; k < 3; ++k) {
    values[kSphereValues + 9 * j + 3 * k] = vertices[k].x;
    values[kSphereValues + 9 * j + 3 * k + 1] = vertices[k].y;
    values[kSphereValues + 9 * j + 3 * k + 2] = vertices[k].z;
  }
}

inline spec::Sphere sphere_of(const Values& values, std::size_t i) {
  return spec::Sphere{spec::Vec3{values[4 * i], values[4 * i + 1], values[4 * i + 2]},
                      values[4 * i + 3]};
}

inline spec::Triangle triangle_of(const Values& values, std::size_t j) {
  const std::size_t at = kSphereValues + 9 * j;
  return spec::Triangle{spec::Vec3{values[at], values[at + 1], values[at + 2]},
                        spec::Vec3{values[at + 3], values[at + 4], values[at + 5]},
                        spec::Vec3{values[at + 6], values[at + 7], values[at + 8]}};
}

inline void put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t k = 0; k < 4; ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

inline void put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t k = 0; k < 8; ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

inline std::uint32_t get32(const Bytes& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    value |= std::to_integer<std::uint32_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

inline std::uint64_t get64(const Bytes& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t k = 0; k < 8; ++k) {
    value |= std::to_integer<std::uint64_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

struct Pixel {
  double t = spec::kNoHitDistance;
  std::uint32_t primitive = spec::kNoHit;
};

/// The nearest primitive of every pixel, found primitive by primitive.
inline std::array<Pixel, spec::kPixels> trace(const Values& values) {
  std::array<Pixel, spec::kPixels> pixels{};
  for (std::size_t i = 0; i < spec::kPrimitives; ++i) {
    for (std::size_t p = 0; p < spec::kPixels; ++p) {
      const spec::Ray ray = spec::pixel_ray(static_cast<std::uint32_t>(p % spec::kWidth),
                                            static_cast<std::uint32_t>(p / spec::kWidth));
      const double t = i < spec::kSpheres
                           ? spec::ray_sphere(ray, sphere_of(values, i))
                           : spec::ray_triangle(ray, triangle_of(values, i - spec::kSpheres));
      if (t < pixels[p].t) {
        pixels[p] = Pixel{t, static_cast<std::uint32_t>(i)};
      }
    }
  }
  return pixels;
}

/// The output the specification gives for the scene at tile edge `edge`.
inline Bytes render(const Values& values, std::uint32_t edge = spec::kDefaultTileEdge) {
  const auto pixels = trace(values);
  Bytes output(spec::kOutputBytes, std::byte{0});
  for (std::size_t p = 0; p < spec::kPixels; ++p) {
    put64(output, spec::pixel_offset(p), std::bit_cast<std::uint64_t>(pixels[p].t));
    put32(output, spec::pixel_offset(p) + 8, pixels[p].primitive);
  }
  const std::size_t per_row = spec::kWidth / edge;
  for (std::size_t k = 0; k < spec::tile_count(edge); ++k) {
    double sum = 0.0;
    for (std::size_t dy = 0; dy < edge; ++dy) {
      for (std::size_t dx = 0; dx < edge; ++dx) {
        const std::size_t p = (k / per_row * edge + dy) * spec::kWidth + k % per_row * edge + dx;
        if (pixels[p].primitive != spec::kNoHit) {
          sum += pixels[p].t;
        }
      }
    }
    put64(output, spec::tile_sum_offset(k), std::bit_cast<std::uint64_t>(sum));
  }
  return output;
}

inline contracts::OracleCase unit_of(std::uint64_t index, const Values& values,
                                     std::uint32_t edge = spec::kDefaultTileEdge) {
  return contracts::OracleCase{index, bytes_of(values), render(values, edge)};
}

/// The number of pixels a scene's rays meet.
inline std::size_t hits_of(const Values& values) {
  const auto pixels = trace(values);
  std::size_t hits = 0;
  for (const Pixel& pixel : pixels) {
    hits += pixel.primitive != spec::kNoHit ? 1 : 0;
  }
  return hits;
}

}  // namespace loadforge::test::raytrace

#endif
