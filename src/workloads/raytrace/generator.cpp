// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/raytrace/generator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "primitives/rng/philox.hpp"
#include "workloads/raytrace/spec.hpp"

namespace loadforge::workloads::raytrace {
namespace {

constexpr std::size_t kTriangleDoublesAt = kSpheres * kSphereDoubles;

[[nodiscard]] std::uint64_t draw(std::uint64_t seed, std::uint64_t index, std::size_t e) noexcept {
  return rng::u64_of(rng::block_at(seed, index, e));
}

void put(std::span<std::byte, kInputBytes> input, std::size_t slot, double value) noexcept {
  std::memcpy(input.data() + slot * sizeof(double), &value, sizeof(double));
}

[[nodiscard]] double get(std::span<const std::byte, kInputBytes> input, std::size_t slot) noexcept {
  double value = 0.0;
  std::memcpy(&value, input.data() + slot * sizeof(double), sizeof(double));
  return value;
}

}  // namespace

void generate_scene(std::uint64_t seed, std::uint64_t index,
                    std::span<std::byte, kInputBytes> input) noexcept {
  for (std::size_t i = 0; i < kSpheres; ++i) {
    const std::size_t first = i * kSphereDoubles;
    put(input, first, position_xy(draw(seed, index, first)));
    put(input, first + 1, position_xy(draw(seed, index, first + 1)));
    put(input, first + 2, position_z(draw(seed, index, first + 2)));
    put(input, first + 3, radius_of(draw(seed, index, first + 3)));
  }
  for (std::size_t j = 0; j < kTriangles; ++j) {
    const std::size_t first_draw = kSphereDraws + j * kTriangleDraws;
    const std::array<double, 3> base = {position_xy(draw(seed, index, first_draw)),
                                        position_xy(draw(seed, index, first_draw + 1)),
                                        position_z(draw(seed, index, first_draw + 2))};
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t offset_draw = first_draw + 3 + 3 * vertex + c;
        put(input, kTriangleDoublesAt + j * kTriangleDoubles + 3 * vertex + c,
            base.at(c) + offset_of(draw(seed, index, offset_draw)));
      }
    }
  }
}

void decode_scene(std::span<const std::byte, kInputBytes> input, SceneStorage& scene) noexcept {
  for (std::size_t i = 0; i < kSpheres; ++i) {
    const std::size_t first = i * kSphereDoubles;
    scene.spheres.at(i) =
        Sphere{Vec3{get(input, first), get(input, first + 1), get(input, first + 2)},
               get(input, first + 3)};
  }
  for (std::size_t j = 0; j < kTriangles; ++j) {
    const std::size_t first = kTriangleDoublesAt + j * kTriangleDoubles;
    scene.triangles.at(j) =
        Triangle{Vec3{get(input, first), get(input, first + 1), get(input, first + 2)},
                 Vec3{get(input, first + 3), get(input, first + 4), get(input, first + 5)},
                 Vec3{get(input, first + 6), get(input, first + 7), get(input, first + 8)}};
  }
}

}  // namespace loadforge::workloads::raytrace
