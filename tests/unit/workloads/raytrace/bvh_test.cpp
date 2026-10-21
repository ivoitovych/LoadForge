// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hierarchy's structure: a binary tree over every primitive once, each box
// holding what is under it, a shape that depends on how many primitives there
// are and not on their values, and a build that is total over any numbers.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "oracle/workloads/raytrace/reference.hpp"
#include "workloads/raytrace/generator.hpp"
#include "workloads/raytrace/scene.hpp"

namespace loadforge::tests::raytrace {
namespace {

std::unique_ptr<rt::SceneStorage> built(const Bytes& input, double margin = rt::kBoundMargin) {
  auto scene = std::make_unique<rt::SceneStorage>();
  rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
  rt::build_bvh(*scene, margin);
  return scene;
}

Bytes generated(std::uint64_t seed, std::uint64_t index) {
  Bytes input(rt::kInputBytes);
  rt::generate_scene(seed, index, std::span<std::byte, rt::kInputBytes>(input));
  return input;
}

bool inside(const rt::Node& outer, const rt::Node& inner) {
  return outer.lo.x <= inner.lo.x && outer.lo.y <= inner.lo.y && outer.lo.z <= inner.lo.z &&
         outer.hi.x >= inner.hi.x && outer.hi.y >= inner.hi.y && outer.hi.z >= inner.hi.z;
}

/// The primitives under node `at`, in hierarchy order, with the leaves visited
/// and the depth reached; checks the tree's own invariants on the way.
// NOLINTNEXTLINE(misc-no-recursion): the tree is eight levels deep
void walk(const rt::SceneStorage& scene, std::uint32_t at, std::size_t depth,
          std::vector<std::uint32_t>& primitives, std::size_t& leaves, std::size_t& deepest) {
  const rt::Node& node = scene.nodes.at(at);
  deepest = std::max(deepest, depth);
  if (node.count > 0) {
    ++leaves;
    EXPECT_LE(node.count, rt::kLeafSize);
    for (std::uint32_t slot = node.link; slot < node.link + node.count; ++slot) {
      primitives.push_back(scene.order.at(slot));
    }
    return;
  }
  ASSERT_LT(node.link, scene.node_count);
  EXPECT_GT(node.link, at + 1) << "the right child comes after the whole left subtree";
  EXPECT_TRUE(inside(node, scene.nodes.at(at + 1))) << "left of node " << at;
  EXPECT_TRUE(inside(node, scene.nodes.at(node.link))) << "right of node " << at;
  walk(scene, at + 1, depth + 1, primitives, leaves, deepest);
  walk(scene, node.link, depth + 1, primitives, leaves, deepest);
}

TEST(RaytraceBvh, EveryPrimitiveIsInExactlyOneLeafAndTheShapeIsTheHalvingTree) {
  for (std::uint64_t unit = 0; unit < 8; ++unit) {
    const auto scene = built(generated(unit + 1, unit));
    std::vector<std::uint32_t> primitives;
    std::size_t leaves = 0;
    std::size_t deepest = 0;
    walk(*scene, 0, 0, primitives, leaves, deepest);
    std::vector<std::uint32_t> sorted = primitives;
    std::ranges::sort(sorted);
    for (std::uint32_t i = 0; i < rt::kPrimitives; ++i) {
      ASSERT_EQ(sorted.at(i), i) << "unit " << unit;
    }
    EXPECT_EQ(leaves, 64U) << "256 primitives halved down to leaves of 4";
    EXPECT_EQ(scene->node_count, 127U);
    EXPECT_EQ(deepest + 1, rt::tree_levels(rt::kPrimitives));
    EXPECT_LE(rt::tree_levels(rt::kPrimitives) + 1, rt::kStackDepth);
  }
}

TEST(RaytraceBvh, TheOrderArrayHoldsTheLeavesInTheOrderTheyAreWalked) {
  const auto scene = built(generated(2, 2));
  std::vector<std::uint32_t> primitives;
  std::size_t leaves = 0;
  std::size_t deepest = 0;
  walk(*scene, 0, 0, primitives, leaves, deepest);
  for (std::size_t slot = 0; slot < rt::kPrimitives; ++slot) {
    EXPECT_EQ(primitives.at(slot), scene->order.at(slot));
  }
}

TEST(RaytraceBvh, EveryLeafBoxHoldsItsPrimitivesGrownByTheMargin) {
  const Bytes input = generated(8, 8);
  const auto scene = built(input);
  std::vector<std::uint32_t> primitives;
  std::size_t leaves = 0;
  std::size_t deepest = 0;
  walk(*scene, 0, 0, primitives, leaves, deepest);
  for (std::uint32_t at = 0; at < scene->node_count; ++at) {
    const rt::Node& node = scene->nodes.at(at);
    if (node.count == 0) {
      continue;
    }
    for (std::uint32_t slot = node.link; slot < node.link + node.count; ++slot) {
      const std::uint32_t id = scene->order.at(slot);
      if (id < rt::kSpheres) {
        const rt::Sphere sphere = sphere_at(input, id);
        EXPECT_LE(node.lo.x, sphere.centre.x - sphere.radius - rt::kBoundMargin);
        EXPECT_GE(node.hi.z, sphere.centre.z + sphere.radius + rt::kBoundMargin);
      } else {
        const rt::Triangle tri = triangle_at(input, id - rt::kSpheres);
        for (const rt::Vec3* v : {&tri.v0, &tri.v1, &tri.v2}) {
          EXPECT_LE(node.lo.y, v->y - rt::kBoundMargin);
          EXPECT_GE(node.hi.x, v->x + rt::kBoundMargin);
        }
      }
    }
  }
}

TEST(RaytraceBvh, ANegativeMarginShrinksTheRootAndAPositiveOneGrowsIt) {
  const Bytes input = generated(9, 9);
  const auto shrunk = built(input, -0.01);
  const auto exact = built(input, 0.0);
  const auto grown = built(input, 0.01);
  EXPECT_LT(shrunk->nodes.at(0).hi.x, exact->nodes.at(0).hi.x);
  EXPECT_GT(grown->nodes.at(0).hi.x, exact->nodes.at(0).hi.x);
  EXPECT_LT(grown->nodes.at(0).lo.z, exact->nodes.at(0).lo.z);
}

TEST(RaytraceBvh, AllPrimitivesIdenticalLeavesTheOrderAsTheIndices) {
  // Every centroid equal on every axis: every key ties, and the index decides.
  Bytes input(rt::kInputBytes);
  for (std::size_t i = 0; i < rt::kSpheres; ++i) {
    set_double(input, i * 4, 0.5);
    set_double(input, i * 4 + 1, 0.5);
    set_double(input, i * 4 + 2, -1.0);
    set_double(input, i * 4 + 3, 0.1);
  }
  for (std::size_t j = 0; j < rt::kTriangles; ++j) {
    const std::size_t first = rt::kSpheres * 4 + j * 9;
    for (std::size_t c = 0; c < 9; ++c) {
      set_double(input, first + c, static_cast<double>(c % 3) * 0.25);
    }
  }
  // Spheres and triangles have different centroids; force both groups to tie
  // among themselves: the spheres' order must then be ascending.
  const auto scene = built(input);
  std::uint32_t previous_sphere = 0;
  auto previous_triangle = static_cast<std::uint32_t>(rt::kSpheres);
  bool first_sphere = true;
  bool first_triangle = true;
  for (const std::uint32_t id : scene->order) {
    if (id < rt::kSpheres) {
      EXPECT_TRUE(first_sphere || id > previous_sphere);
      previous_sphere = id;
      first_sphere = false;
    } else {
      EXPECT_TRUE(first_triangle || id > previous_triangle);
      previous_triangle = id;
      first_triangle = false;
    }
  }
}

TEST(RaytraceBvh, NumbersThatAreNotNumbersBuildAnEmptyTreeOfTheSameShape) {
  const Bytes input(rt::kInputBytes, std::byte{0xFF});  // every double a NaN
  const auto scene = built(input);
  EXPECT_EQ(scene->node_count, 127U);
  const rt::Node& root = scene->nodes.at(0);
  EXPECT_EQ(root.lo.x, std::numeric_limits<double>::infinity());
  EXPECT_EQ(root.hi.x, -std::numeric_limits<double>::infinity());
}

TEST(RaytraceBvh, InfinitiesAndNegativeRadiiStillMakeAPermutationOfTheSameShape) {
  Bytes input(rt::kInputBytes);
  for (std::size_t slot = 0; slot < rt::kInputBytes / 8; ++slot) {
    set_double(input, slot, slot % 2 == 0 ? std::numeric_limits<double>::infinity() : -0.5);
  }
  const auto scene = built(input);
  EXPECT_EQ(scene->node_count, 127U);
  std::vector<std::uint32_t> order(scene->order.begin(), scene->order.end());
  std::ranges::sort(order);
  for (std::uint32_t i = 0; i < rt::kPrimitives; ++i) {
    ASSERT_EQ(order.at(i), i);
  }
}

TEST(RaytraceBvh, TheWidestCentroidAxisSplitsTheRoot) {
  // Primitives spread along one axis only: the root's children are separated
  // along it, and the other two axes are shared.
  for (std::size_t axis = 0; axis < 3; ++axis) {
    Bytes input(rt::kInputBytes);
    for (std::size_t i = 0; i < rt::kSpheres; ++i) {
      std::array<double, 3> centre = {0.0, 0.0, -1.0};
      centre.at(axis) = static_cast<double>(i) * 0.01 - 0.64;
      for (std::size_t c = 0; c < 3; ++c) {
        set_double(input, i * 4 + c, centre.at(c));
      }
      set_double(input, i * 4 + 3, 0.01);
    }
    for (std::size_t j = 0; j < rt::kTriangles; ++j) {
      std::array<double, 3> base = {0.0, 0.0, -1.0};
      base.at(axis) = static_cast<double>(j) * 0.01 - 0.64;
      for (std::size_t v = 0; v < 3; ++v) {
        for (std::size_t c = 0; c < 3; ++c) {
          set_double(input, rt::kSpheres * 4 + j * 9 + v * 3 + c,
                     base.at(c) + (c == axis ? 0.001 * static_cast<double>(v) : 0.0));
        }
      }
    }
    const auto scene = built(input);
    const rt::Node& root = scene->nodes.at(0);
    const rt::Node& left = scene->nodes.at(1);
    const rt::Node& right = scene->nodes.at(root.link);
    const auto along = [axis](const rt::Vec3& v) {
      const std::array<double, 3> parts = {v.x, v.y, v.z};
      return parts.at(axis);
    };
    // Spheres and triangles interleave along the axis, so the halves overlap
    // by at most a primitive's width; their centres are what separate.
    EXPECT_LT(along(left.lo), along(right.lo)) << "axis " << axis;
    EXPECT_LT(along(left.hi), along(right.hi)) << "axis " << axis;
  }
}

}  // namespace
}  // namespace loadforge::tests::raytrace
