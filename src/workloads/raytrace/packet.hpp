// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_PACKET_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_PACKET_HPP

// Traversal of the hierarchy by a packet of kLanes rays, written once over a
// Packet policy, and the scalar policy. The wide policy is wide.hpp.
//
// WHY A PACKET CAN SHARE A TRAVERSAL AND STILL BE EXACT
// -----------------------------------------------------
// A node is visited when any lane of the packet may reach it, and every
// primitive of a visited leaf is tried by every lane, including a lane whose
// own box test would have skipped it. That costs time and cannot change an
// answer: a lane's result is the nearest of the primitives it tried under the
// specification's arithmetic and tie rule, and trying one primitive more is
// what brute force does for all of them. What may NOT happen is a lane
// skipping a primitive that brute force would have given it as nearest, which
// is the box test's burden: it is conservative by kBoundMargin.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>

#include "workloads/raytrace/scene.hpp"

namespace loadforge::workloads::raytrace {

/// Whether a candidate (t, id) replaces the best so far: it must be a hit and
/// less than the best by (t, then primitive index), so an equal t goes to the
/// lower index whatever order the hierarchy visits them in.
[[nodiscard]] inline bool replaces(double t, std::uint32_t id, double best_t,
                                   std::uint32_t best_id) noexcept {
  const bool hit = t < kNoHitDistance;
  const bool nearer = std::tie(t, id) < std::tie(best_t, best_id);
  return hit & nearer;
}

/// The record of one pixel: t, the primitive, four zero bytes.
inline void write_record(std::span<std::byte> out, double t, std::uint32_t id) noexcept {
  const std::uint32_t zero = 0;
  std::memcpy(out.data(), &t, sizeof(t));
  std::memcpy(out.data() + sizeof(t), &id, sizeof(id));
  std::memcpy(out.data() + sizeof(t) + sizeof(id), &zero, sizeof(zero));
}

/// The ray of pixel (x, y) and the reciprocals of its direction, which are
/// finite and nonzero for every pixel (the components are odd multiples of
/// 1/256 and -1).
struct LaneRay {
  Ray ray;
  Vec3 inverse;
};

[[nodiscard]] inline LaneRay lane_ray(std::uint32_t x, std::uint32_t y) noexcept {
  const Ray ray = pixel_ray(x, y);
  return LaneRay{ray, Vec3{1.0 / ray.direction.x, 1.0 / ray.direction.y, 1.0 / ray.direction.z}};
}

/// Depth-first, left child first, until every node a lane might reach has been
/// visited. Nodes are read from `scene`; a leaf is handed to the packet.
template <typename Packet>
[[gnu::always_inline]] inline void trace_packet(const SceneStorage& scene,
                                                Packet& packet) noexcept {
  std::array<std::uint32_t, kStackDepth> stack{};
  std::size_t top = 0;
  stack.at(top++) = 0;
  while (top > 0) {
    const std::uint32_t at = stack.at(--top);
    const Node& node = scene.nodes.at(at);
    if (!packet.reaches(node)) {
      continue;
    }
    if (node.count > 0) {
      packet.leaf(scene, node);
    } else {
      stack.at(top++) = node.link;
      stack.at(top++) = at + 1;
    }
  }
}

/// Every pixel of row y, in packets of kLanes adjacent pixels.
template <typename Packet>
[[gnu::always_inline]] inline void trace_row(const SceneStorage& scene, std::uint32_t y,
                                             std::span<std::byte, kRowBytes> row) noexcept {
  for (std::uint32_t x = 0; x < kWidth; x += static_cast<std::uint32_t>(kLanes)) {
    Packet packet(x, y);
    trace_packet(scene, packet);
    packet.write(row.subspan(x * kPixelBytes, kLanes * kPixelBytes));
  }
}

/// One packet, one lane at a time, with the specification's own functions.
/// The reference every wide kernel is held to.
class ScalarPacket {
 public:
  ScalarPacket(std::uint32_t x0, std::uint32_t y) noexcept {
    for (std::uint32_t lane = 0; lane < kLanes; ++lane) {
      lanes_.at(lane) = lane_ray(x0 + lane, y);
    }
    best_.fill(kNoHitDistance);
    best_id_.fill(kNoHit);
  }

  [[nodiscard]] bool reaches(const Node& node) const noexcept {
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      if (lane_reaches(lanes_.at(lane), node, best_.at(lane))) {
        return true;
      }
    }
    return false;
  }

  void leaf(const SceneStorage& scene, const Node& node) noexcept {
    for (std::uint32_t slot = node.link; slot < node.link + node.count; ++slot) {
      const std::uint32_t id = scene.order.at(slot);
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        const Ray& ray = lanes_.at(lane).ray;
        const double t = id < kSpheres ? ray_sphere(ray, scene.spheres.at(id))
                                       : ray_triangle(ray, scene.triangles.at(id - kSpheres));
        if (replaces(t, id, best_.at(lane), best_id_.at(lane))) {
          best_.at(lane) = t;
          best_id_.at(lane) = id;
        }
      }
    }
  }

  void write(std::span<std::byte> out) const noexcept {
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      write_record(out.subspan(lane * kPixelBytes, kPixelBytes), best_.at(lane),
                   best_id_.at(lane));
    }
  }

 private:
  /// The slab test, per axis the interval of t inside the box, and whether the
  /// intervals meet at t >= 0 before the best hit so far. `t_near <= t_far` and
  /// `t_near <= best` fail for a NaN, so a box that is not a box is skipped.
  [[nodiscard]] static bool lane_reaches(const LaneRay& lane, const Node& node,
                                         double best_t) noexcept {
    double t_near = 0.0;
    double t_far = kNoHitDistance;
    const auto slab = [&t_near, &t_far](double lo, double hi, double origin, double inverse) {
      const double t0 = (lo - origin) * inverse;
      const double t1 = (hi - origin) * inverse;
      t_near = greater_of(t_near, lesser_of(t0, t1));
      t_far = lesser_of(t_far, greater_of(t0, t1));
    };
    slab(node.lo.x, node.hi.x, lane.ray.origin.x, lane.inverse.x);
    slab(node.lo.y, node.hi.y, lane.ray.origin.y, lane.inverse.y);
    slab(node.lo.z, node.hi.z, lane.ray.origin.z, lane.inverse.z);
    const bool within = t_near <= t_far;
    const bool before = t_near <= best_t;
    return within & before;
  }

  std::array<LaneRay, kLanes> lanes_{};
  std::array<double, kLanes> best_{};
  std::array<std::uint32_t, kLanes> best_id_{};
};

}  // namespace loadforge::workloads::raytrace

#endif
