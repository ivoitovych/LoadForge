// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <tuple>

#include "workloads/raytrace/scene.hpp"

namespace loadforge::workloads::raytrace {
namespace {

[[nodiscard]] double axis_of(const Vec3& v, std::size_t axis) noexcept {
  const std::array<double, 3> parts = {v.x, v.y, v.z};
  return parts.at(axis);
}

struct PrimBox {
  Vec3 lo;
  Vec3 hi;
  Vec3 centre;
};

[[nodiscard]] PrimBox box_of(const Sphere& sphere, double margin) noexcept {
  // r * r is all the intersection arithmetic uses of the radius, so a negative
  // one is a sphere of its magnitude.
  const double reach = std::fabs(sphere.radius);
  const Vec3& c = sphere.centre;
  return PrimBox{Vec3{c.x - reach - margin, c.y - reach - margin, c.z - reach - margin},
                 Vec3{c.x + reach + margin, c.y + reach + margin, c.z + reach + margin}, c};
}

[[nodiscard]] PrimBox box_of(const Triangle& tri, double margin) noexcept {
  const auto low = [](double a, double b, double c) { return lesser_of(lesser_of(a, b), c); };
  const auto high = [](double a, double b, double c) { return greater_of(greater_of(a, b), c); };
  const auto mid = [](double a, double b, double c) { return ((a + b) + c) / 3.0; };
  return PrimBox{
      Vec3{low(tri.v0.x, tri.v1.x, tri.v2.x) - margin, low(tri.v0.y, tri.v1.y, tri.v2.y) - margin,
           low(tri.v0.z, tri.v1.z, tri.v2.z) - margin},
      Vec3{high(tri.v0.x, tri.v1.x, tri.v2.x) + margin, high(tri.v0.y, tri.v1.y, tri.v2.y) + margin,
           high(tri.v0.z, tri.v1.z, tri.v2.z) + margin},
      Vec3{mid(tri.v0.x, tri.v1.x, tri.v2.x), mid(tri.v0.y, tri.v1.y, tri.v2.y),
           mid(tri.v0.z, tri.v1.z, tri.v2.z)}};
}

/// A key whose unsigned order is the total order of the doubles (negative
/// ones below positive ones, a NaN somewhere fixed), so that sorting on it is
/// well defined whatever the values are.
[[nodiscard]] std::uint64_t order_key(double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const std::uint64_t mask = (std::uint64_t{0} - (bits >> 63U)) | 0x8000000000000000ULL;
  return bits ^ mask;
}

class Builder {
 public:
  Builder(SceneStorage& scene, double margin) noexcept : scene_(&scene) {
    for (std::size_t i = 0; i < kSpheres; ++i) {
      boxes_.at(i) = box_of(scene.spheres.at(i), margin);
    }
    for (std::size_t j = 0; j < kTriangles; ++j) {
      boxes_.at(kSpheres + j) = box_of(scene.triangles.at(j), margin);
    }
    std::iota(scene.order.begin(), scene.order.end(), std::uint32_t{0});
  }

  /// Builds the node over order[begin, end) and returns its index. Recursive,
  /// to the depth of the halving tree, which scene.hpp bounds at compile time.
  // NOLINTNEXTLINE(misc-no-recursion)
  std::uint32_t build(std::uint32_t begin, std::uint32_t end) noexcept {
    const std::uint32_t self = next_node_++;
    constexpr double kInf = kNoHitDistance;
    Node node;
    node.lo = Vec3{kInf, kInf, kInf};
    node.hi = Vec3{-kInf, -kInf, -kInf};
    Vec3 centre_lo = node.lo;
    Vec3 centre_hi = node.hi;
    for (std::uint32_t slot = begin; slot < end; ++slot) {
      const PrimBox& box = boxes_.at(scene_->order.at(slot));
      node.lo = Vec3{lesser_of(node.lo.x, box.lo.x), lesser_of(node.lo.y, box.lo.y),
                     lesser_of(node.lo.z, box.lo.z)};
      node.hi = Vec3{greater_of(node.hi.x, box.hi.x), greater_of(node.hi.y, box.hi.y),
                     greater_of(node.hi.z, box.hi.z)};
      centre_lo = Vec3{lesser_of(centre_lo.x, box.centre.x), lesser_of(centre_lo.y, box.centre.y),
                       lesser_of(centre_lo.z, box.centre.z)};
      centre_hi = Vec3{greater_of(centre_hi.x, box.centre.x), greater_of(centre_hi.y, box.centre.y),
                       greater_of(centre_hi.z, box.centre.z)};
    }
    if (end - begin <= kLeafSize) {
      node.link = begin;
      node.count = end - begin;
      scene_->nodes.at(self) = node;
      return self;
    }
    // The widest axis of the centroids; the lowest on a tie or when no
    // extent is a number.
    const std::array<double, 3> extent = {centre_hi.x - centre_lo.x, centre_hi.y - centre_lo.y,
                                          centre_hi.z - centre_lo.z};
    const auto axis =
        static_cast<std::size_t>(std::distance(extent.begin(), std::ranges::max_element(extent)));
    // Ties on the key fall to the primitive index, so no two primitives
    // compare equal and the order is the same on every library.
    std::sort(scene_->order.begin() + begin, scene_->order.begin() + end,
              [this, axis](std::uint32_t lhs, std::uint32_t rhs) noexcept {
                const std::uint64_t lhs_key = key_of(lhs, axis);
                const std::uint64_t rhs_key = key_of(rhs, axis);
                return std::tie(lhs_key, lhs) < std::tie(rhs_key, rhs);
              });
    const std::uint32_t middle = begin + (end - begin) / 2;
    build(begin, middle);  // the left child is the next node, self + 1
    node.link = build(middle, end);
    node.count = 0;
    scene_->nodes.at(self) = node;
    return self;
  }

  [[nodiscard]] std::uint32_t nodes_built() const noexcept { return next_node_; }

 private:
  [[nodiscard]] std::uint64_t key_of(std::uint32_t primitive, std::size_t axis) const noexcept {
    return order_key(axis_of(boxes_.at(primitive).centre, axis));
  }

  SceneStorage* scene_;
  std::array<PrimBox, kPrimitives> boxes_{};
  std::uint32_t next_node_ = 0;
};

}  // namespace

void build_bvh(SceneStorage& scene, double margin) noexcept {
  Builder builder(scene, margin);
  builder.build(0, static_cast<std::uint32_t>(kPrimitives));
  scene.node_count = builder.nodes_built();
}

}  // namespace loadforge::workloads::raytrace
