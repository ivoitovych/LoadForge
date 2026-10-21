// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_ADAPTIVE_MESH_SPEC_HPP
#define LOADFORGE_WORKLOADS_ADAPTIVE_MESH_SPEC_HPP

// CONTRACT (ADR 0004). The specification the adaptive mesh workload and its
// independent oracle are both written from (verification.md §3; determinism.md
// §2, BoundedResidual; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The adaptive mesh stream writes the stress implementation: a quadtree
//     refined around the unit's hotspots, its leaves claimed by the worker's
//     threads from a shared queue, in whatever order they finish
//     (src/workloads/adaptive_mesh/).
//   The wave 4 oracle stream writes the oracle: the same mesh built by a
//     plain recursion, and the sampled coarse cells summed in one fixed
//     order (src/verification/oracle/adaptive_mesh/) -- from this file, never
//     from the workload.
//
// WHAT THE WORKLOAD IS
// --------------------
// A unit is a field on the unit square, sampled on a quadtree that is refined
// wherever the unit's hotspots are, and integrated over each of 16 x 16 coarse
// cells:
//     calculate -> detect the active regions (the cells with a hotspot in
//     them) -> subdivide them -> calculate the leaves -> gather the leaves
//     into their coarse cell, in the order the threads finish.
// The structure (which cells split) is integer logic, identical on every run
// and every machine. The leaf values are fixed sequences of binary64
// operations, identical on every run and every machine. The ONE thing that
// varies is the order in which a coarse cell's leaves are added up, because a
// thread that finishes early adds its leaf first: that is the dynamic load
// balancing which makes this workload kBoundedResidual (determinism.md §2),
// and it is why its check has a derived tolerance and its single-threaded
// reproduction path is a bounded comparison, not a bit comparison.
// The workload's pull request carries the justification for the class; this
// specification fixes what it varies and by how much.
//
// A UNIT
// ------
//   input   kHotspots hotspots, each two little-endian u32: x then y, the
//           position on the finest lattice, kInputBytes in all. Hotspot h is
//           hotspot_of(u64_of(block_at(seed, index, h))): x the top 9 bits of
//           the draw and y the next 9 bits, so both are integers in
//           [0, kLattice) = [0, 512), every value attained, and the 46 low
//           bits are discarded. The same cell may hold several hotspots.
//   output  kOutputBytes bytes, little-endian throughout:
//             [0, kCountsOffset)           kCoarseCells doubles: the integral of
//                                          coarse cell c at byte 8 c
//             [kCountsOffset, kOutputBytes) kCoarseCells u32: the number of
//                                          leaves of coarse cell c at byte
//                                          kCountsOffset + 4 c
//           Coarse cell c = cy * kBaseCells + cx, with cx and cy, both in
//           [0, kBaseCells), the cell's ix and iy at level kBaseLevel.
//
// THE MESH (exact: integers only)
// -------------------------------
// A cell is (level, ix, iy), covering [ix 2^-level, (ix + 1) 2^-level) x
// [iy 2^-level, (iy + 1) 2^-level) of the unit square. The coarse cells are
// the cells of level kBaseLevel = 4. A cell SPLITS iff its level is below
// kMaxLevel = 9 and cell_contains() holds for some hotspot: the hotspot's
// finest-lattice position lies inside it. A split cell has four children, of
// level + 1, child q = 0 .. 3 being (2 ix + (q & 1), 2 iy + (q >> 1)). The
// LEAVES of a coarse cell are the cells under it (itself included) that do
// not split. A hotspot with a coordinate outside [0, kLattice) is in no cell.
// A coarse cell has between 1 and kMaxLeavesPerCell = 4^5 = 1024 leaves (all
// cells down to level 9); a generated unit has about 3700 leaves in all,
// at most about 50 in a cell (measured), and the work of one unit is bounded
// whatever its input: at most 9 cells split per hotspot, so at most 6913
// leaves in all.
// The LEAF ORDER of a coarse cell is depth first, the children of a split cell
// in the order q = 0, 1, 2, 3.
//
// THE VALUES (exact operation sequences)
// --------------------------------------
// For a leaf (level, ix, iy), with
//     point = Point{cell_centre(level, ix), cell_centre(level, iy)}
// and hotspot k:
//     term_k   = hotspot_term(point, hotspot_k)
//     f        = (((+0.0 + term_0) + term_1) + ... ) + term_{kHotspots - 1}
//                -- hotspots in index order, plain binary64 addition;
//     x_leaf   = f * leaf_area(level)
// Everything in a term except the division is EXACT (the centres are multiples
// of 2^-10, so the differences, their squares, their sum, the scaling by
// kSharpness = 2^12 and the + 1 all fit in 53 bits), so a fused multiply-add
// changes nothing there; the division and the sum over hotspots round, once
// each, and the order of the sum is fixed. A lane-parallel path therefore
// runs each lane over its OWN leaf, sequentially over the hotspots; a path
// that splits the sum over hotspots between lanes is not a path of this
// workload. x_leaf is exact: leaf_area() is a power of two. Every term is in
// (2^-13, 1], so f is in (2^-5, 256] and every x_leaf is positive, above
// 2^-23 (the smallest leaf area is 2^-18) and at most 256.
// The values are bit-identical on every ISA path and architecture. Division
// and sum are the only roundings; neither is replaced by a reciprocal.
//
// The INTEGRAL of coarse cell c is the sum over its m leaves of x_leaf, added
// in ANY order, by any grouping of partial sums, with or without
// compensation: that freedom is the whole of the workload's non-determinism.
//
// DETERMINISM
// -----------
// Class kBoundedResidual; oracle contract Contract::kBounded. The leaf counts
// are exact; the integrals agree between runs, thread counts and the oracle
// only within cell_tolerance(). There is no decomposition: the plan reports 1
// and a configured value other than 0 or 1 is refused as kConfiguration. A
// unit is computed by one executor call.
//
// THE CHECK, AND ITS BOUND
// ------------------------
// Notation: u = 2^-53 (kUnitRoundoff), gamma_k = k u / (1 - k u) (Higham,
// Accuracy and Stability of Numerical Algorithms, 2nd ed., section 3.1).
// For a coarse cell of m leaves, x_1 .. x_m are the leaves' x_leaf, each
// positive, E = x_1 + ... + x_m exactly (so the sum of the |x_i| is E).
//   1. The workload's value c: every x_i takes part in at most m - 1
//      additions whatever the order and grouping, so |c - E| <= gamma_{m-1} E
//      (Higham section 4.2; compensated summation is no worse).
//   2. The oracle's value c': the leaves added in the LEAF ORDER, one by one,
//      starting from +0.0 -- plain binary64 addition -- so |c' - E| <=
//      gamma_{m-1} E, and c' is one fixed double for the cell.
//   3. So |c - c'| <= 2 gamma_{m-1} E, and E <= c' / (1 - gamma_{m-1}), so
//      |c - c'| <= 2 gamma_{m-1} c' / (1 - gamma_{m-1}). The right side grows
//      with gamma, and gamma_m >= gamma_{m-1}, so with gamma_m (which keeps
//      the bound positive at m = 1, where the true error is zero) it is
//      T = 2 gamma_m c' / (1 - gamma_m) = g c' / (1 - g), g = 2 m u.
//   4. The subtraction c - c' is exact when it matters: g <= 2^-42, so
//      T < c' / 2, and if |c - c'| <= T the two are within a factor of two
//      of each other, where binary64 subtraction is exact (Sterbenz); if they
//      are farther apart, |c - c'| >= c' / 2 > T by a wide margin, and
//      rounding of the difference cannot bring it to T. The residual
//      |c - c'|, computed in binary64, is compared with T faithfully.
//   5. T itself is computed, in this order, as cell_tolerance(m, c'):
//          g   = (2 m) * u                       exact (2 m <= 2048)
//          den = 1 - g                           exact (a multiple of 2^-52
//                                                 in [1/2, 1))
//          tau = ((g * c') / den) * kToleranceSlack
//      kToleranceSlack = 1 + 2^-40. The product, the quotient and the last
//      product each round once, a relative error of at most u each, so
//          tau / T in [(1 + 2^-40)(1 - u)^3, (1 + 2^-40)(1 + u)^3]
//                   within [1 + 2^-41, 1 + 2^-39].
//      The slack is there so that tau is never below T whatever the rounding
//      of its computation (2^-40 is about 2^13 times the three roundings
//      together): a correct result is never rejected. It is also small
//      enough to test: the oracle's tests breach the bound at (1 + epsilon)
//      times tau and hold it at (1 - epsilon) times tau. A residual is
//      stored as a double, so it moves in steps of ulp(c') <= 2^-52 c' <=
//      tau / m: epsilon cannot be smaller than 1 / m, and the cells for
//      which that is small are those with many leaves -- build an input
//      whose hotspots crowd one coarse cell (a hand-made input is as
//      legal as a generated one), up to kMaxLeavesPerCell leaves.
//
// WHAT THE ORACLE ANSWERS
// -----------------------
// Each verdict is on the unit's output given its input (oracle.hpp), and is
// kBounded: BoundedMismatch, never ExactMismatch.
//   1. Evidence whose input is not kInputBytes or whose output is not
//      kOutputBytes: NotChecked, blocks_compared 0.
//   2. A hotspot coordinate not below kLattice: NotChecked, blocks_compared
//      0. (The mesh built from the stress side would disagree with the
//      generated one, and the input-integrity bracket reports the corrupt
//      input itself.)
//   3. The ITEMS, in this order: the leaf counts of coarse cells 0 ..
//      kCoarseCells - 1; then the integrals of the kSampledCells sampled
//      coarse cells, sample j = 0 .. kSampledCells - 1 being a cell with an
//      index in [8 j, 8 j + 8), chosen as a function of the unit index alone
//      (so the samples are distinct and spread over the whole square). The
//      oracle evaluates the items in order and STOPS at the first that
//      fails; the failure named is that one, and blocks_compared counts the
//      items evaluated, the failing one included. (An Agreement counts
//      kCoarseCells + kSampledCells.)
//        * A leaf count item passes when the stored u32 equals the count the
//          mesh above gives. If not: BoundedMismatch, check kCheckLeafCount, a
//          space and the decimal cell index; residual |stored - expected|
//          as a double; tolerance kCountTolerance (0.5: counts are
//          integers, and any difference is at least 1); element the cell
//          index.
//        * An integral item passes when |c - c'| <= tau, with m the cell's
//          leaf count (already known right at this point), c the stored
//          double and c' the oracle's value (step 2), tau = cell_tolerance(m,
//          c'). If not -- including a stored NaN or infinity, whose residual
//          is NaN or infinite -- BoundedMismatch, check kCheckIntegral, a
//          space and the decimal cell index; residual the double |c - c'|;
//          tolerance tau; element the cell index; condition_number absent.
//
// VERSIONS
// --------
// kVersion versions this specification: the format, the arithmetic and the
// checks. A change to it is a contract change (ADR 0004), and both streams
// update. The workload's own bits are versioned separately by its
// Descriptor::version, which changes when the bits a configuration produces
// change; a change to the specification that changes bits changes both.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::adaptive_mesh {

inline constexpr std::string_view kName = "adaptive_mesh";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::uint32_t kBaseLevel = 4;
inline constexpr std::uint32_t kMaxLevel = 9;
inline constexpr std::uint32_t kBaseCells = 16;  ///< coarse cells along one side: 2^kBaseLevel
inline constexpr std::uint32_t kLattice = 512;   ///< finest cells along one side: 2^kMaxLevel
inline constexpr std::size_t kCoarseCells = std::size_t{kBaseCells} * kBaseCells;
inline constexpr std::size_t kHotspots = 256;
inline constexpr std::size_t kHotspotBytes = 8;
inline constexpr std::size_t kInputBytes = kHotspots * kHotspotBytes;
inline constexpr std::size_t kCountsOffset = kCoarseCells * sizeof(double);
inline constexpr std::size_t kOutputBytes = kCountsOffset + kCoarseCells * sizeof(std::uint32_t);
/// 4^(kMaxLevel - kBaseLevel): every cell under a coarse cell split down to the finest level.
inline constexpr std::uint32_t kMaxLeavesPerCell = 1024;
inline constexpr std::size_t kSampledCells = 32;

/// The sharpness of a hotspot's term, 2^12.
inline constexpr double kSharpness = 4096.0;
/// u = 2^-53.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
/// 1 + 2^-40.
inline constexpr double kToleranceSlack = 1.0 + 1.0 / 1099511627776.0;
/// The tolerance of a leaf count: 0.5, below any difference between integers.
inline constexpr double kCountTolerance = 0.5;

/// How a failed item's check begins; the decimal index of the coarse cell
/// follows after one space.
inline constexpr std::string_view kCheckLeafCount = "leaf count of coarse cell";
inline constexpr std::string_view kCheckIntegral = "integral of coarse cell";

/// Byte offset in the output of coarse cell `c`'s integral, c < kCoarseCells.
[[nodiscard]] constexpr std::size_t integral_offset(std::size_t c) noexcept {
  return c * sizeof(double);
}

/// Byte offset in the output of coarse cell `c`'s leaf count, c < kCoarseCells.
[[nodiscard]] constexpr std::size_t count_offset(std::size_t c) noexcept {
  return kCountsOffset + c * sizeof(std::uint32_t);
}

/// A hotspot's position on the finest lattice.
struct Hotspot {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

/// The hotspot a 64-bit draw makes: x the top 9 bits, y the next 9. Both
/// below kLattice.
[[nodiscard]] constexpr Hotspot hotspot_of(std::uint64_t bits) noexcept {
  constexpr std::uint64_t kNineBits = 0x1FFU;
  return Hotspot{static_cast<std::uint32_t>(bits >> 55U),
                 static_cast<std::uint32_t>((bits >> 46U) & kNineBits)};
}

/// A cell of the quadtree, and a point of the unit square.
struct Cell {
  std::uint32_t level = 0;
  std::uint32_t ix = 0;
  std::uint32_t iy = 0;
};

struct Point {
  double x = 0.0;
  double y = 0.0;
};

/// Whether the cell holds the hotspot: both the hotspot's coordinates shifted
/// down to the cell's level are the cell's. A coordinate not below kLattice
/// shifts to 2^level or more, so it is in no cell that exists.
/// Preconditions: level <= kMaxLevel, and ix and iy below 2^level.
[[nodiscard]] constexpr bool cell_contains(const Cell& cell, const Hotspot& hotspot) noexcept {
  const std::uint32_t shift = kMaxLevel - cell.level;
  return (hotspot.x >> shift) == cell.ix && (hotspot.y >> shift) == cell.iy;
}

/// The centre of cell index `i` along one axis at `level`: (2 i + 1) / 2^(level + 1).
/// Exact. A hotspot's centre is the same function at kMaxLevel.
[[nodiscard]] constexpr double cell_centre(std::uint32_t level, std::uint64_t i) noexcept {
  return static_cast<double>(2 * i + 1) / static_cast<double>(std::uint64_t{1} << (level + 1));
}

/// A leaf's area, 4^-level, as a fraction of the unit square. Exact.
[[nodiscard]] constexpr double leaf_area(std::uint32_t level) noexcept {
  return 1.0 / static_cast<double>(std::uint64_t{1} << (2 * level));
}

/// One hotspot's term at a point: with the hotspot's centre
/// (hx, hy) = (cell_centre(kMaxLevel, x), cell_centre(kMaxLevel, y)),
///   dx = point.x - hx;  dy = point.y - hy;
///   d2 = dx dx + dy dy;  den = 1 + kSharpness d2;  term = 1 / den.
/// Every step but the division is exact for centres of the lattice.
[[nodiscard]] constexpr double hotspot_term(const Point& point, const Hotspot& hotspot) noexcept {
  const double dx = point.x - cell_centre(kMaxLevel, hotspot.x);
  const double dy = point.y - cell_centre(kMaxLevel, hotspot.y);
  const double d2 = dx * dx + dy * dy;
  return 1.0 / (1.0 + kSharpness * d2);
}

/// tau, the tolerance of a coarse cell's integral: see "THE CHECK, AND ITS
/// BOUND", step 5. Preconditions: 1 <= leaves <= kMaxLeavesPerCell, and
/// `reference` (c') finite and positive.
// The pair is a count and a value, named for what they are and passed in the order the
// specification writes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] constexpr double cell_tolerance(std::uint32_t leaves, double reference) noexcept {
  const double g = static_cast<double>(2U * leaves) * kUnitRoundoff;
  return ((g * reference) / (1.0 - g)) * kToleranceSlack;
}

}  // namespace loadforge::workloads::adaptive_mesh

#endif
