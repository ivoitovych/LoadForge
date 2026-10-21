// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_SCENE_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_SCENE_HPP

// A unit's scene, decoded, and the hierarchy over it. Internal to the ray
// tracing workload: nothing outside src/workloads/raytrace/ includes this.
//
// The hierarchy is binary, built by halving: a range of primitives larger than
// kLeafSize is cut at its middle after sorting on the widest axis of its
// centroids, so the shape depends only on how many primitives there are, never
// on their values. That is what bounds the traversal stack for any input, a
// corrupt one included (WorkloadContract: compute() is total).

#include <array>
#include <cstddef>
#include <cstdint>

#include "workloads/raytrace/spec.hpp"

namespace loadforge::workloads::raytrace {

/// The most primitives a leaf holds.
inline constexpr std::size_t kLeafSize = 4;
/// Every node box is grown by this much on every side (2^-30), so that a box
/// test rounded the wrong way by the slab arithmetic cannot skip a primitive
/// the exact intersection arithmetic would hit. The arithmetic's error is
/// about 10^-14 on coordinates of this scale; this is five orders above it.
inline constexpr double kBoundMargin = 1.0 / 1073741824.0;
/// Nodes of a binary tree with kPrimitives leaves, at most.
inline constexpr std::size_t kMaxNodes = 2 * kPrimitives - 1;
/// Rays that share a traversal.
inline constexpr std::size_t kLanes = 4;
/// Bytes of one row of records.
inline constexpr std::size_t kRowBytes = kWidth * kPixelBytes;
inline constexpr std::size_t kStackDepth = 16;

/// Levels of the halving tree over `count` primitives.
[[nodiscard]] constexpr std::size_t tree_levels(std::size_t count) noexcept {
  std::size_t levels = 1;
  while (count > kLeafSize) {
    count = count - count / 2;
    ++levels;
  }
  return levels;
}

// Depth-first traversal pushes both children of each interior node it leaves
// behind, so the stack holds at most one more entry than the tree has levels.
static_assert(tree_levels(kPrimitives) + 1 <= kStackDepth);
static_assert(kPixels % kLanes == 0 && kWidth % kLanes == 0);

/// One node. An interior node's left child is the next node; `link` is its
/// right child and `count` is 0. A leaf holds `count` primitives from slot
/// `link` of the order array.
struct Node {
  Vec3 lo;
  Vec3 hi;
  std::uint32_t link = 0;
  std::uint32_t count = 0;
};

struct SceneStorage {
  std::array<Sphere, kSpheres> spheres{};
  std::array<Triangle, kTriangles> triangles{};
  std::array<Node, kMaxNodes> nodes{};
  /// Primitive indices, in hierarchy order.
  std::array<std::uint32_t, kPrimitives> order{};
  std::uint32_t node_count = 0;
};

/// The lesser and the greater of two doubles, a NaN being neither: a number
/// that is not a number adds nothing to a bound, which is also what brute
/// force makes of it (every comparison in spec.hpp fails on a NaN).
[[nodiscard]] inline double lesser_of(double lhs, double rhs) noexcept {
  return rhs < lhs ? rhs : lhs;
}
[[nodiscard]] inline double greater_of(double lhs, double rhs) noexcept {
  return rhs > lhs ? rhs : lhs;
}

/// Builds the hierarchy over scene.spheres and scene.triangles into
/// scene.nodes and scene.order, every box grown by `margin`. A negative margin
/// shrinks them: production passes kBoundMargin and a test passes a negative
/// one to show a hierarchy that skips what it should have tried disagrees with
/// brute force.
void build_bvh(SceneStorage& scene, double margin) noexcept;

}  // namespace loadforge::workloads::raytrace

#endif
