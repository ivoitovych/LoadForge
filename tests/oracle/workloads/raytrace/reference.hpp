// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_RAYTRACE_REFERENCE_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_RAYTRACE_REFERENCE_HPP

// What the ray tracing workload's tests hold it to: the image as
// src/workloads/raytrace/spec.hpp defines it, by brute force. Every primitive
// is tried for every pixel in index order and the nearest kept, a later
// primitive replacing the best only if its t is strictly less. It shares the
// specification's arithmetic (ray_sphere, ray_triangle) and nothing of the
// workload's hierarchy, packets or kernels. It is written here, in the
// workload's own tests, and is not the independent oracle of
// src/verification/oracle/raytrace (another stream's); the digests of
// tests/fixtures/workloads/raytrace/golden.txt, made by a Python model of the
// specification, are what the C++ is checked against from outside.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "workloads/raytrace/spec.hpp"

namespace loadforge::tests::raytrace {

namespace rt = loadforge::workloads::raytrace;

using Bytes = std::vector<std::byte>;

[[nodiscard]] inline double double_at(std::span<const std::byte> bytes, std::size_t slot) {
  double value = 0.0;
  std::memcpy(&value, bytes.data() + slot * sizeof(double), sizeof(value));
  return value;
}

inline void set_double(std::span<std::byte> bytes, std::size_t slot, double value) {
  std::memcpy(bytes.data() + slot * sizeof(double), &value, sizeof(value));
}

[[nodiscard]] inline rt::Sphere sphere_at(std::span<const std::byte> input, std::size_t i) {
  const std::size_t first = i * rt::kSphereDoubles;
  return rt::Sphere{rt::Vec3{double_at(input, first), double_at(input, first + 1),
                             double_at(input, first + 2)},
                    double_at(input, first + 3)};
}

[[nodiscard]] inline rt::Triangle triangle_at(std::span<const std::byte> input, std::size_t j) {
  const std::size_t first = rt::kSpheres * rt::kSphereDoubles + j * rt::kTriangleDoubles;
  const auto vertex = [&](std::size_t k) {
    return rt::Vec3{double_at(input, first + 3 * k), double_at(input, first + 3 * k + 1),
                    double_at(input, first + 3 * k + 2)};
  };
  return rt::Triangle{vertex(0), vertex(1), vertex(2)};
}

struct Hit {
  double t = rt::kNoHitDistance;
  std::uint32_t primitive = rt::kNoHit;
};

/// The nearest primitive for the ray of pixel (x, y): every primitive, in
/// index order.
[[nodiscard]] inline Hit brute_force_pixel(std::span<const std::byte> input, std::uint32_t x,
                                           std::uint32_t y) {
  const rt::Ray ray = rt::pixel_ray(x, y);
  Hit best;
  for (std::size_t i = 0; i < rt::kPrimitives; ++i) {
    const double t = i < rt::kSpheres ? rt::ray_sphere(ray, sphere_at(input, i))
                                      : rt::ray_triangle(ray, triangle_at(input, i - rt::kSpheres));
    if (t < best.t) {
      best.t = t;
      best.primitive = static_cast<std::uint32_t>(i);
    }
  }
  return best;
}

inline void put_record(std::span<std::byte> out, std::size_t pixel, const Hit& hit) {
  const std::uint32_t zero = 0;
  std::byte* at = out.data() + rt::pixel_offset(pixel);
  std::memcpy(at, &hit.t, sizeof(hit.t));
  std::memcpy(at + 8, &hit.primitive, sizeof(hit.primitive));
  std::memcpy(at + 12, &zero, sizeof(zero));
}

/// Tile `tile`'s sum from stored records, as the specification states it.
[[nodiscard]] inline double tile_sum_of(std::span<const std::byte> output, std::uint32_t edge,
                                        std::size_t tile) {
  const std::size_t per_row = rt::kWidth / edge;
  const std::size_t first_x = (tile % per_row) * edge;
  const std::size_t first_y = (tile / per_row) * edge;
  double sum = 0.0;
  for (std::size_t y = first_y; y < first_y + edge; ++y) {
    for (std::size_t x = first_x; x < first_x + edge; ++x) {
      const std::size_t at = rt::pixel_offset(y * rt::kWidth + x);
      std::uint32_t primitive = 0;
      std::memcpy(&primitive, output.data() + at + 8, sizeof(primitive));
      if (primitive != rt::kNoHit) {
        sum = sum + double_at(output.subspan(at), 0);
      }
    }
  }
  return sum;
}

/// The whole output for `input` at tile edge `edge`.
[[nodiscard]] inline Bytes brute_force_output(std::span<const std::byte> input,
                                              std::uint32_t edge) {
  Bytes out(rt::kOutputBytes, std::byte{0});
  for (std::uint32_t y = 0; y < rt::kHeight; ++y) {
    for (std::uint32_t x = 0; x < rt::kWidth; ++x) {
      put_record(out, y * rt::kWidth + x, brute_force_pixel(input, x, y));
    }
  }
  for (std::size_t tile = 0; tile < rt::tile_count(edge); ++tile) {
    set_double(std::span<std::byte>(out).subspan(rt::kTileSumsOffset), tile,
               tile_sum_of(out, edge, tile));
  }
  return out;
}

/// What a comparison of an output with brute force answered.
enum class Verdict : std::uint8_t {
  kAgreement,
  /// The output differs from brute force.
  kMismatch,
  /// The comparison could not be made; never a pass.
  kNotPerformed,
};

struct Comparison {
  Verdict verdict = Verdict::kNotPerformed;
  /// Pixels whose 16-byte record differs, and tiles whose 8-byte sum differs.
  std::vector<std::size_t> pixels;
  std::vector<std::size_t> tiles;
};

[[nodiscard]] inline Comparison compare_with_brute_force(std::span<const std::byte> input,
                                                         std::span<const std::byte> output,
                                                         std::uint32_t edge) {
  Comparison comparison;
  if (input.size() != rt::kInputBytes || output.size() != rt::kOutputBytes) {
    return comparison;  // kNotPerformed
  }
  const Bytes expected = brute_force_output(input, edge);
  for (std::size_t p = 0; p < rt::kPixels; ++p) {
    const std::size_t at = rt::pixel_offset(p);
    if (std::memcmp(expected.data() + at, output.data() + at, rt::kPixelBytes) != 0) {
      comparison.pixels.push_back(p);
    }
  }
  for (std::size_t k = 0; k < rt::kMaxTiles; ++k) {
    const std::size_t at = rt::tile_sum_offset(k);
    if (std::memcmp(expected.data() + at, output.data() + at, sizeof(double)) != 0) {
      comparison.tiles.push_back(k);
    }
  }
  comparison.verdict = comparison.pixels.empty() && comparison.tiles.empty() ? Verdict::kAgreement
                                                                           : Verdict::kMismatch;
  return comparison;
}

}  // namespace loadforge::tests::raytrace

#endif
