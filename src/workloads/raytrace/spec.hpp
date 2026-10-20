// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_SPEC_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_SPEC_HPP

// CONTRACT (ADR 0004). The specification the ray tracing workload and its
// independent oracle are both written from (verification.md §3; determinism.md;
// IMPLEMENTATION M4). Frozen: changed only by a contract change, never inside
// a stream.
//
//   The ray tracing stream writes the stress implementation: a bounding
//     volume hierarchy over the unit's primitives, traversed by packets of
//     rays, with scalar, AVX2 and NEON paths behind the dispatch seam
//     (src/workloads/raytrace/).
//   The wave 4 oracle stream writes the oracle: brute-force ray against every
//     primitive for sampled pixels (src/verification/oracle/raytrace/) --
//     from this file, never from the hierarchy.
//
// WHAT IS SHARED, AND WHAT IS NOT
// -------------------------------
// The two sides share the ARITHMETIC and keep their ALGORITHMS apart. The
// arithmetic -- how a unit's numbers are drawn, which camera ray a pixel
// shoots, the exact sequence of IEEE 754 operations that decides whether a ray
// meets a sphere or a triangle -- is defined once, as code, below, because two
// careful readers of prose give two last bits, and a last bit that differs is
// a false corruption report, the one failure this tool cannot afford. What the
// workload adds to it, a hierarchy that skips most primitives, is the thing
// under test; the oracle's answer is the definition of the right image: every
// primitive tried, in index order, the nearest kept. A hierarchy that skips a
// primitive it should have tried (a box test that rounds the wrong way, a leaf
// visited in the wrong order, a stale bound) disagrees with it, and that is
// what the oracle is for. Neither side may call the other's code; both call
// the helpers of this header.
//
// A UNIT: ONE FRAME OF A SYNTHETIC SCENE
// --------------------------------------
//   Scene   kSpheres spheres, then kTriangles triangles: kPrimitives in all.
//           Primitive i < kSpheres is sphere i; primitive kSpheres + j is
//           triangle j. The index is the primitive's identity everywhere
//           below.
//   Camera  fixed, and not part of the input: a pinhole at kEye looking down
//           -z, kWidth x kHeight pixels. Pixel (x, y), x to the right and y
//           downward, both from 0, shoots the ray origin kEye, direction
//           pixel_direction(x, y). The direction is not normalised, and t
//           below is the multiple of it.
//   Image   for each pixel, the nearest primitive the ray meets and where.
//
//   input   (kSpheres * kSphereDoubles + kTriangles * kTriangleDoubles)
//           little-endian IEEE 754 doubles, kInputBytes in all:
//             sphere i,   i in [0, kSpheres):    cx cy cz r
//             triangle j, j in [0, kTriangles):  v0x v0y v0z v1x v1y v1z v2x v2y v2z
//           spheres first, each in index order, then triangles.
//   output  kOutputBytes bytes, little-endian throughout:
//             [0, kTileSumsOffset)   kPixels records of kPixelBytes, pixel
//                                    p = y * kWidth + x at byte p * kPixelBytes:
//                                      [0, 8)    t, a double
//                                      [8, 12)   the primitive's index, a u32
//                                      [12, 16)  zero
//             [kTileSumsOffset, end) kMaxTiles doubles, the tile sums.
//
// HOW THE INPUT IS GENERATED
// --------------------------
// Draw e of unit `index` is u64_of(block_at(seed, index, e)) (primitives/rng/
// philox.hpp); `bits` below is one draw. The decoders are the helpers
// lattice(), position_xy(), position_z(), radius_of() and offset_of(), each
// exact: every result is a multiple of a power of two with at most 24
// significant bits, so no rounding happens in decoding, and the ranges below
// are exactly the ranges the decoders produce (the top 20 bits of a draw are
// used and the rest discarded).
//
//   sphere i      draws 4 i .. 4 i + 3:
//                   cx = position_xy(d0), cy = position_xy(d1),
//                   cz = position_z(d2),  r  = radius_of(d3)
//   triangle j    draws kSphereDraws + 12 j .. + 11:
//                   base (bx, by, bz) = position_xy(d0), position_xy(d1),
//                                       position_z(d2)
//                   vertex k in {0, 1, 2}, component c in {x, y, z} (0, 1, 2):
//                                       base_c + offset_of(d[3 + 3 k + c])
//                   the sum is of two doubles that are multiples of 2^-21 and
//                   so is exact.
//
// RANGES, exactly (closed intervals; each end is attained by some draw):
//   sphere cx, cy     kSphereXy   [-2, 2 - 2^-18]
//   sphere cz         kSphereZ    [-4, -2^-18]
//   sphere r          kRadius     [2^-4, 3 * 2^-4 - 2^-23]   (below 0.1875)
//   vertex x, y       kVertexXy   [-2.25, 2.25 - 2^-18 - 2^-21]
//   vertex z          kVertexZ    [-4.25, 0.25 - 2^-18 - 2^-21]
// Every value is finite. The eye (z = 4) is outside every primitive, and every
// primitive lies in z < 0.25, so every ray that meets one does so at t >= 3.75:
// there are no hits behind the eye, no rays starting inside a primitive and no
// secondary rays. A generated scene hits between a tenth and nine tenths of
// its pixels (measured: about 45%); the workload's T4 tests hold it to that.
//
// THE ARITHMETIC (the order is part of the specification)
// -------------------------------------------------------
// Every operation is a binary64 operation rounded to nearest, ties to even,
// performed in the order the helpers below write it. Specifically:
//   * no fused multiply-add anywhere, whether the compiler contracts or a
//     kernel names an intrinsic: dot(a, b) is ((ax bx + ay by) + az bz) with
//     each product rounded before it is added;
//   * a division is a division: no multiplication by a reciprocal, no
//     reciprocal estimate, no reciprocal square root;
//   * a SIMD path evaluates the same sequence in each lane, so its bits equal
//     the scalar path's. A path that cannot is not a path of this workload.
// Square root and division are correctly rounded by IEEE 754, so the results
// are the same on x86-64 and ARM64 (determinism.md §3).
//
// The nearest primitive for a ray is the one with the least t among
// ray_sphere() and ray_triangle() over all kPrimitives, tried in index order,
// a later primitive replacing the best only if its t is STRICTLY less: a tie
// goes to the lower index. A t of kNoHitDistance (+infinity) is a miss. A pixel
// no primitive meets is the record t = kNoHitDistance (bits 0x7FF0000000000000),
// primitive kNoHit.
//
// THE TILE SUMS
// -------------
// The image is cut into square tiles of `decomposition` pixels a side, the
// tile edge. Tile k = (y / edge) * (kWidth / edge) + (x / edge), tiles numbered
// from 0 in row-major order. Tile sum k is
//     s = +0.0; for each pixel of the tile -- rows from the top, left to right
//     within a row -- whose record is a hit: s = s + t;
// a plain left-to-right binary64 sum, from the pixel records the unit wrote
// (so it is a function of the records, not of anything else in the unit). A
// tile without a hit is +0.0. Slots k >= tile_count(edge) are zero bytes.
// A pixel record is a hit when its primitive is not kNoHit.
//
// DETERMINISM
// -----------
// Class: kBitExactFixedDecomposition, for the tile sums, whose bits depend on
// the tile edge; the decomposition is the tile edge, one of kTileEdges,
// default kDefaultTileEdge (a configured 0 resolves to the default; any other
// value is refused as kConfiguration). The same bits for one tile edge at any
// thread count, on any run, on every ISA path and on both architectures: the
// pixel records do not depend on the tile edge at all, and the tile sums are
// sums in a fixed order that a lane-parallel reduction cannot reorder, so this
// specification gives more than the class names. The class is declared as the
// roadmap names it; whether it should be kBitExact is recorded as a question
// in docs/JOURNAL.md. The oracle's contract is therefore Contract::kExact,
// and no tolerance exists anywhere in this specification.
// A unit is traced by one executor call; how the rays are spread over the
// threads is the workload's.
//
// WHAT THE ORACLE ANSWERS
// -----------------------
// Each verdict is on the unit's output given its input (oracle.hpp), and is
// kExact: ExactMismatch, never BoundedMismatch.
//   1. Evidence whose input is not kInputBytes or whose output is not
//      kOutputBytes: NotChecked, blocks_compared 0.
//   2. An input value outside its range above, or not finite (a NaN, an
//      infinity): NotChecked, blocks_compared 0. The scene is then not one
//      this specification generates, the hierarchy over it is not required to
//      agree with brute force, and the input-integrity bracket reports the
//      corrupt input itself.
//   3. The ITEMS, in this order: first the kSampledPixels sampled pixels, then
//      the tile sums of tiles 0 .. tile_count(edge) - 1, the edge being the
//      plan's decomposition. The oracle evaluates the items in order and
//      STOPS at the first that fails; the failure named is that one, and
//      blocks_compared counts the items evaluated, the failing one included.
//      (An Agreement counts kSampledPixels + tile_count(edge).)
//        * The sampled pixels are kSampledPixels distinct pixels, a function
//          of the unit index alone. Sample j, j = 0 .. kSampledPixels - 1, is
//          a pixel inside tile j of the 8 x 8 tiling, in row-major order of
//          that tiling, so every region of the image is sampled whichever edge
//          is configured. Sample j is evaluated in order of j. Its item
//          passes when the 16 stored bytes of the pixel's record equal, byte
//          for byte, the record the brute-force trace produces.
//        * A tile sum's item passes when the stored 8 bytes equal the sum
//          recomputed from the STORED records of the tile's pixels. A stored
//          record that is not well-formed (below) makes the item fail
//          without a difference.
//   A failed pixel item is an ExactMismatch whose check is kCheckPixel, a
//   space and the decimal pixel index p, and whose difference is in
//   Domain::kStored over the item's 16 bytes: offsets from the START OF THE
//   OUTPUT (first_offset and last_offset the first and last differing byte
//   of the item), popcount the differing bits of the item, expected the
//   brute-force record's bytes and actual the stored ones, from window_offset
//   to the end of the item.
//   A failed tile item is an ExactMismatch whose check is kCheckTileSum, a
//   space and the decimal tile index k; with a difference over the sum's
//   8 bytes in the same terms when all the tile's records are well-formed,
//   and none (difference absent) when one is not; the check's text after the
//   index then names the first malformed pixel.
// A record is well-formed when its bytes [12, 16) are zero and either its
// primitive is kNoHit and its t has the bits of kNoHitDistance, or its
// primitive is below kPrimitives and its t is finite and greater than 0.
//
// VERSIONS
// --------
// kVersion versions this specification: the format, the arithmetic and the
// checks. A change to it is a contract change (ADR 0004), and both streams
// update. The workload's own bits are versioned separately by its
// Descriptor::version, which changes when the bits a configuration produces
// change; a change to the specification that changes bits changes both.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace loadforge::workloads::raytrace {

inline constexpr std::string_view kName = "raytrace";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kWidth = 64;
inline constexpr std::size_t kHeight = 64;
inline constexpr std::size_t kPixels = kWidth * kHeight;
inline constexpr std::size_t kSpheres = 128;
inline constexpr std::size_t kTriangles = 128;
inline constexpr std::size_t kPrimitives = kSpheres + kTriangles;
inline constexpr std::size_t kSphereDoubles = 4;
inline constexpr std::size_t kTriangleDoubles = 9;
/// Draws that give the spheres: four each. The triangles' draws follow, twelve each.
inline constexpr std::size_t kSphereDraws = 4 * kSpheres;
inline constexpr std::size_t kTriangleDraws = 12;
inline constexpr std::size_t kInputBytes =
    (kSpheres * kSphereDoubles + kTriangles * kTriangleDoubles) * sizeof(double);

inline constexpr std::size_t kPixelBytes = 16;
inline constexpr std::size_t kMaxTiles = 64;
inline constexpr std::size_t kTileSumsOffset = kPixels * kPixelBytes;
inline constexpr std::size_t kOutputBytes = kTileSumsOffset + kMaxTiles * sizeof(double);

/// The primitive index of a pixel no primitive meets.
inline constexpr std::uint32_t kNoHit = 0xFFFFFFFFU;
/// The t of a miss: +infinity, bits 0x7FF0000000000000.
inline constexpr double kNoHitDistance = std::numeric_limits<double>::infinity();
inline constexpr std::uint64_t kNoHitDistanceBits = 0x7FF0000000000000ULL;

/// The tile edges, in pixels, that divide the image into at most kMaxTiles tiles.
inline constexpr std::array<std::uint32_t, 3> kTileEdges = {8, 16, 32};
inline constexpr std::uint32_t kDefaultTileEdge = 16;
inline constexpr std::size_t kSampledPixels = 64;

/// How a failed item's check begins; the decimal index of the pixel or the
/// tile follows after one space.
inline constexpr std::string_view kCheckPixel = "pixel";
inline constexpr std::string_view kCheckTileSum = "tile sum";

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_tile_edge(std::uint32_t edge) noexcept {
  return std::ranges::any_of(kTileEdges, [edge](std::uint32_t known) { return known == edge; });
}

/// The tiles an image cut at `edge` has. Precondition: is_tile_edge(edge).
[[nodiscard]] constexpr std::size_t tile_count(std::uint32_t edge) noexcept {
  const std::size_t per_row = kWidth / edge;
  return per_row * (kHeight / edge);
}

/// Byte offset in the output of pixel `p`'s record, p < kPixels.
[[nodiscard]] constexpr std::size_t pixel_offset(std::size_t p) noexcept { return p * kPixelBytes; }

/// Byte offset in the output of tile `k`'s sum, k < kMaxTiles.
[[nodiscard]] constexpr std::size_t tile_sum_offset(std::size_t k) noexcept {
  return kTileSumsOffset + k * sizeof(double);
}

// ---- decoding the draws ---------------------------------------------------

/// The top 20 bits of a draw, scaled: a multiple of 2^-18 in [0, 4 - 2^-18].
[[nodiscard]] constexpr double lattice(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus18 = 1.0 / 262144.0;
  return static_cast<double>(bits >> 44U) * kTwoToMinus18;
}

/// x and y of a centre or a base point: lattice - 2, in kSphereXy.
[[nodiscard]] constexpr double position_xy(std::uint64_t bits) noexcept {
  return lattice(bits) - 2.0;
}

/// z of a centre or a base point: lattice - 4, in kSphereZ.
[[nodiscard]] constexpr double position_z(std::uint64_t bits) noexcept {
  return lattice(bits) - 4.0;
}

/// A sphere's radius: 2^-4 plus a multiple of 2^-23, in kRadius.
[[nodiscard]] constexpr double radius_of(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus23 = 1.0 / 8388608.0;
  return 0.0625 + static_cast<double>(bits >> 44U) * kTwoToMinus23;
}

/// A triangle vertex's offset from its base: a multiple of 2^-21 in
/// [-0.25, 0.25 - 2^-21].
[[nodiscard]] constexpr double offset_of(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus21 = 1.0 / 2097152.0;
  return static_cast<double>(bits >> 44U) * kTwoToMinus21 - 0.25;
}

/// A closed interval of doubles; a NaN is in none.
struct Interval {
  double lo = 0.0;
  double hi = 0.0;
};

[[nodiscard]] constexpr bool contains(const Interval& interval, double value) noexcept {
  return value >= interval.lo && value <= interval.hi;
}

inline constexpr Interval kSphereXy = {-2.0, 2.0 - 1.0 / 262144.0};
inline constexpr Interval kSphereZ = {-4.0, -1.0 / 262144.0};
inline constexpr Interval kRadius = {0.0625, 0.1875 - 1.0 / 8388608.0};
inline constexpr Interval kVertexXy = {-2.25, 2.25 - 1.0 / 262144.0 - 1.0 / 2097152.0};
inline constexpr Interval kVertexZ = {-4.25, 0.25 - 1.0 / 262144.0 - 1.0 / 2097152.0};

// ---- the arithmetic -------------------------------------------------------

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Sphere {
  Vec3 centre;
  double radius = 0.0;
};

struct Triangle {
  Vec3 v0;
  Vec3 v1;
  Vec3 v2;
};

struct Ray {
  Vec3 origin;
  Vec3 direction;
};

/// The pinhole.
inline constexpr Vec3 kEye = {0.0, 0.0, 4.0};

/// The direction of the ray through the centre of pixel (x, y), x and y below
/// 64: ((2x - 63) / 256, (63 - 2y) / 256, -1). Exact.
[[nodiscard]] constexpr Vec3 pixel_direction(std::uint32_t x, std::uint32_t y) noexcept {
  constexpr double kTwoToMinus8 = 1.0 / 256.0;
  return Vec3{(static_cast<double>(x) * 2.0 - 63.0) * kTwoToMinus8,
              (63.0 - static_cast<double>(y) * 2.0) * kTwoToMinus8, -1.0};
}

/// The ray of pixel (x, y): from kEye, in pixel_direction(x, y).
[[nodiscard]] constexpr Ray pixel_ray(std::uint32_t x, std::uint32_t y) noexcept {
  return Ray{kEye, pixel_direction(x, y)};
}

[[nodiscard]] constexpr Vec3 sub(const Vec3& lhs, const Vec3& rhs) noexcept {
  return Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

/// ((ax bx + ay by) + az bz), each product rounded before it is added.
[[nodiscard]] constexpr double dot(const Vec3& lhs, const Vec3& rhs) noexcept {
  return (lhs.x * rhs.x + lhs.y * rhs.y) + lhs.z * rhs.z;
}

/// (ay bz - az by, az bx - ax bz, ax by - ay bx), each product rounded before
/// the subtraction.
[[nodiscard]] constexpr Vec3 cross(const Vec3& lhs, const Vec3& rhs) noexcept {
  return Vec3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
              lhs.x * rhs.y - lhs.y * rhs.x};
}

/// The nearer root of the ray against a sphere, or kNoHitDistance:
///   oc = origin - centre; a = d.d; hb = oc.d; cc = oc.oc - r r;
///   disc = hb hb - a cc;
///   a miss unless disc >= 0 (a NaN is a miss);
///   t = (-hb - sqrt(disc)) / a;  a hit only if t > 0 (a NaN is a miss).
/// The far root is not tried: the eye is outside every sphere of a generated
/// scene, so the near root is the hit.
[[nodiscard]] inline double ray_sphere(const Ray& ray, const Sphere& sphere) noexcept {
  const Vec3 oc = sub(ray.origin, sphere.centre);
  const double a = dot(ray.direction, ray.direction);
  const double hb = dot(oc, ray.direction);
  const double cc = dot(oc, oc) - sphere.radius * sphere.radius;
  const double disc = hb * hb - a * cc;
  if (!(disc >= 0.0)) {
    return kNoHitDistance;
  }
  const double t = (-hb - std::sqrt(disc)) / a;
  return t > 0.0 ? t : kNoHitDistance;
}

/// The ray against a triangle (Moller-Trumbore), or kNoHitDistance:
///   e1 = v1 - v0; e2 = v2 - v0; p = d x e2; det = e1.p;
///   a miss unless |det| > 0 (a NaN is a miss);
///   inv = 1 / det; s = origin - v0; u = (s.p) inv;
///   a miss unless u >= 0, and a miss unless u <= 1;
///   q = s x e1; v = (d.q) inv;
///   a miss unless v >= 0, and a miss unless u + v <= 1;
///   t = (e2.q) inv;  a hit only if t > 0.
/// Every comparison is written so that a NaN fails it.
[[nodiscard]] inline double ray_triangle(const Ray& ray, const Triangle& triangle) noexcept {
  const Vec3 e1 = sub(triangle.v1, triangle.v0);
  const Vec3 e2 = sub(triangle.v2, triangle.v0);
  const Vec3 p = cross(ray.direction, e2);
  const double det = dot(e1, p);
  if (!(std::fabs(det) > 0.0)) {
    return kNoHitDistance;
  }
  const double inv = 1.0 / det;
  const Vec3 s = sub(ray.origin, triangle.v0);
  const double u = dot(s, p) * inv;
  if (!(u >= 0.0)) {
    return kNoHitDistance;
  }
  if (!(u <= 1.0)) {
    return kNoHitDistance;
  }
  const Vec3 q = cross(s, e1);
  const double v = dot(ray.direction, q) * inv;
  if (!(v >= 0.0)) {
    return kNoHitDistance;
  }
  if (!(u + v <= 1.0)) {
    return kNoHitDistance;
  }
  const double t = dot(e2, q) * inv;
  return t > 0.0 ? t : kNoHitDistance;
}

}  // namespace loadforge::workloads::raytrace

#endif
