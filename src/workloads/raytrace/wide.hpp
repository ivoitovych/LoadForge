// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_WIDE_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_WIDE_HPP

// The wide packet: the four lanes of a packet as one vector of four doubles,
// the same sequence of binary64 operations as spec.hpp's ray_sphere() and
// ray_triangle() in every lane, so its bits equal the scalar path's.
//
// It is written with the compilers' portable vector types, not with
// intrinsics. Built for the machine's baseline, a vector of four doubles is
// two registers of two; inside a function the build marks for AVX2 (x86-64) it
// is one register of four, and on AArch64 it is two NEON registers. The same
// text is therefore each architecture's wide path (kernels.cpp says which).
//
// WHAT KEEPS THE BITS EQUAL TO THE SCALAR PATH
//   * No fused multiply-add: -ffp-contract=off on this target, and the AVX2
//     function is marked for AVX2 alone, not for FMA3.
//   * A division is a division and a square root is a square root, each
//     correctly rounded; the square root is taken lane by lane.
//   * The early returns of the scalar functions are masks here. A lane that
//     the scalar function would have returned from computes the rest on
//     numbers that may be NaN or infinite and its result is thrown away by
//     the mask, so no lane's answer depends on another's.
//   * Every comparison is the ordered one, false for a NaN, as the scalar
//     `!(x >= 0)` forms are.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/raytrace/packet.hpp"

namespace loadforge::workloads::raytrace::wide {

using V = double __attribute__((vector_size(4 * sizeof(double))));
using M = decltype(V{} < V{});

static_assert(sizeof(V) == sizeof(M));
static_assert(sizeof(V) / sizeof(double) == kLanes);

#define LOADFORGE_WIDE_INLINE [[gnu::always_inline]] inline

LOADFORGE_WIDE_INLINE V splat(double value) noexcept { return V{value, value, value, value}; }

LOADFORGE_WIDE_INLINE M bits_of(V value) noexcept { return std::bit_cast<M>(value); }

/// `lhs` where the mask is set, `rhs` where it is not.
LOADFORGE_WIDE_INLINE V select(M mask, V lhs, V rhs) noexcept {
  return std::bit_cast<V>((mask & bits_of(lhs)) | (~mask & bits_of(rhs)));
}

LOADFORGE_WIDE_INLINE V lesser(V lhs, V rhs) noexcept { return select(rhs < lhs, rhs, lhs); }
LOADFORGE_WIDE_INLINE V greater(V lhs, V rhs) noexcept { return select(rhs > lhs, rhs, lhs); }

LOADFORGE_WIDE_INLINE V root(V value) noexcept {
  return V{std::sqrt(value[0]), std::sqrt(value[1]), std::sqrt(value[2]), std::sqrt(value[3])};
}

LOADFORGE_WIDE_INLINE V absolute(V value) noexcept {
  return std::bit_cast<V>(bits_of(value) & (M{} + 0x7FFFFFFFFFFFFFFFLL));
}

struct V3 {
  V x;
  V y;
  V z;
};

LOADFORGE_WIDE_INLINE V3 splat3(const Vec3& v) noexcept {
  return V3{splat(v.x), splat(v.y), splat(v.z)};
}
LOADFORGE_WIDE_INLINE V3 sub(const V3& lhs, const V3& rhs) noexcept {
  return V3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}
/// ((ax bx + ay by) + az bz), each product rounded before it is added.
LOADFORGE_WIDE_INLINE V dot(const V3& lhs, const V3& rhs) noexcept {
  return (lhs.x * rhs.x + lhs.y * rhs.y) + lhs.z * rhs.z;
}
LOADFORGE_WIDE_INLINE V3 cross(const V3& lhs, const V3& rhs) noexcept {
  return V3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

/// ray_sphere() in every lane.
LOADFORGE_WIDE_INLINE V ray_sphere(const V3& origin, const V3& direction,
                                   const Sphere& sphere) noexcept {
  const V3 oc = sub(origin, splat3(sphere.centre));
  const V a = dot(direction, direction);
  const V hb = dot(oc, direction);
  const V cc = dot(oc, oc) - splat(sphere.radius) * splat(sphere.radius);
  const V disc = hb * hb - a * cc;
  const M real = disc >= splat(0.0);
  // The root of a negative number is not taken: its lane is a miss already.
  const V t = (-hb - root(select(real, disc, splat(0.0)))) / a;
  const M hit = real & (t > splat(0.0));
  return select(hit, t, splat(kNoHitDistance));
}

/// ray_triangle() in every lane (Moller-Trumbore).
LOADFORGE_WIDE_INLINE V ray_triangle(const V3& origin, const V3& direction,
                                     const Triangle& triangle) noexcept {
  const V3 e1 = splat3(Vec3{triangle.v1.x - triangle.v0.x, triangle.v1.y - triangle.v0.y,
                            triangle.v1.z - triangle.v0.z});
  const V3 e2 = splat3(Vec3{triangle.v2.x - triangle.v0.x, triangle.v2.y - triangle.v0.y,
                            triangle.v2.z - triangle.v0.z});
  const V3 p = cross(direction, e2);
  const V det = dot(e1, p);
  const M usable = absolute(det) > splat(0.0);
  // 1 / det for the lanes that have one; no lane divides by zero.
  const V inv = splat(1.0) / select(usable, det, splat(1.0));
  const V3 s = sub(origin, splat3(triangle.v0));
  const V u = dot(s, p) * inv;
  const M inside_u = (u >= splat(0.0)) & (u <= splat(1.0));
  const V3 q = cross(s, e1);
  const V v = dot(direction, q) * inv;
  const M inside_v = (v >= splat(0.0)) & ((u + v) <= splat(1.0));
  const V t = dot(e2, q) * inv;
  const M hit = usable & inside_u & inside_v & (t > splat(0.0));
  return select(hit, t, splat(kNoHitDistance));
}

class Packet {
 public:
  [[gnu::always_inline]] Packet(std::uint32_t x0, std::uint32_t y) noexcept {
    std::array<LaneRay, kLanes> rays{};
    for (std::uint32_t lane = 0; lane < kLanes; ++lane) {
      rays.at(lane) = lane_ray(x0 + lane, y);
    }
    origin_ = splat3(rays.at(0).ray.origin);
    direction_ = V3{V{rays.at(0).ray.direction.x, rays.at(1).ray.direction.x,
                      rays.at(2).ray.direction.x, rays.at(3).ray.direction.x},
                    V{rays.at(0).ray.direction.y, rays.at(1).ray.direction.y,
                      rays.at(2).ray.direction.y, rays.at(3).ray.direction.y},
                    V{rays.at(0).ray.direction.z, rays.at(1).ray.direction.z,
                      rays.at(2).ray.direction.z, rays.at(3).ray.direction.z}};
    inverse_ = V3{V{rays.at(0).inverse.x, rays.at(1).inverse.x, rays.at(2).inverse.x,
                    rays.at(3).inverse.x},
                  V{rays.at(0).inverse.y, rays.at(1).inverse.y, rays.at(2).inverse.y,
                    rays.at(3).inverse.y},
                  V{rays.at(0).inverse.z, rays.at(1).inverse.z, rays.at(2).inverse.z,
                    rays.at(3).inverse.z}};
    best_t_ = splat(kNoHitDistance);
    best_id_ = M{} + static_cast<std::int64_t>(kNoHit);
  }

  [[gnu::always_inline]] [[nodiscard]] inline bool reaches(const Node& node) const noexcept {
    V t_near = splat(0.0);
    V t_far = splat(kNoHitDistance);
    const auto slab = [&t_near, &t_far](double lo, double hi, V origin, V inverse) {
      const V t0 = (splat(lo) - origin) * inverse;
      const V t1 = (splat(hi) - origin) * inverse;
      t_near = greater(t_near, lesser(t0, t1));
      t_far = lesser(t_far, greater(t0, t1));
    };
    slab(node.lo.x, node.hi.x, origin_.x, inverse_.x);
    slab(node.lo.y, node.hi.y, origin_.y, inverse_.y);
    slab(node.lo.z, node.hi.z, origin_.z, inverse_.z);
    const M met = (t_near <= t_far) & (t_near <= best_t_);
    // Any lane set: the lanes of a mask are all ones or all zeros.
    return (met[0] | met[1] | met[2] | met[3]) != 0;
  }

  [[gnu::always_inline]] inline void leaf(const SceneStorage& scene, const Node& node) noexcept {
    for (std::uint32_t slot = node.link; slot < node.link + node.count; ++slot) {
      const std::uint32_t id = scene.order.at(slot);
      const V t = id < kSpheres ? ray_sphere(origin_, direction_, scene.spheres.at(id))
                                : ray_triangle(origin_, direction_, scene.triangles.at(id - kSpheres));
      const M id_lanes = M{} + static_cast<std::int64_t>(id);
      const M hit = t < splat(kNoHitDistance);
      const M nearer = (t < best_t_) | ((t == best_t_) & (id_lanes < best_id_));
      const M take = hit & nearer;
      best_t_ = select(take, t, best_t_);
      best_id_ = (take & id_lanes) | (~take & best_id_);
    }
  }

  [[gnu::always_inline]] inline void write(std::span<std::byte> out) const noexcept {
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      write_record(out.subspan(lane * kPixelBytes, kPixelBytes), best_t_[lane],
                   static_cast<std::uint32_t>(best_id_[lane]));
    }
  }

 private:
  V3 origin_{};
  V3 direction_{};
  V3 inverse_{};
  V best_t_{};
  M best_id_{};
};

#undef LOADFORGE_WIDE_INLINE

}  // namespace loadforge::workloads::raytrace::wide

#endif
