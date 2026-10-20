// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_STENCIL_SPEC_HPP
#define LOADFORGE_WORKLOADS_STENCIL_SPEC_HPP

// CONTRACT (ADR 0004). The specification the stencil workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The stencil stream writes the stress implementation: a vectorised, tiled
//     sweep with scalar, AVX2 (FMA3) and NEON kernels behind the dispatch seam
//     (src/workloads/stencil/).
//   The wave-3 oracle stream writes the oracle (a scalar per-cell update on
//     sampled sub-regions, src/verification/oracle/stencil/) and the derived
//     tolerance, from this file and nothing else -- never from the kernels.
//
// VERSIONS. kVersion versions this SPECIFICATION: the unit's format and the
// check below. A change to either increments it. The workload's own bits are
// versioned separately, by Descriptor::version (workload.hpp), which moves
// whenever a configuration's output changes -- including when this
// specification does not.
//
// A UNIT: kSteps JACOBI SWEEPS OF A 5-POINT STENCIL ON AN n x n GRID, n = kEdge
// ------------------------------------------------------------------------------
// Write u(i, j) for the cell in row i and column j, i and j in [0, n).
//
//   input   the grid u0, row-major, n*n little-endian IEEE 754 doubles.
//           Element e of the unit (e = i * n + j) is
//             entry_value(u64_of(block_at(seed, index, e)))
//           (primitives/rng/philox.hpp): a multiple of 2^-52 in [-1, 1 - 2^-52].
//           The lower end IS reached (entry_value(0) == -1.0); the upper end,
//           +1, is not. Every correctly generated cell is finite with
//           magnitude at most 1 -- the tolerance below relies on it. Only the
//           first two words of each Philox block are used.
//   output  the grid u after kSteps sweeps, in the layout of the input.
//
// One sweep reads a grid and writes a new one (Jacobi: a cell is updated from
// the previous sweep's values, never from values already updated in this one).
// The RING -- the cells with i or j equal to 0 or n - 1 -- is fixed
// (Dirichlet): a sweep copies it unchanged, and so the output ring is the
// input ring, bit for bit. An interior cell (1 <= i, j <= n - 2) becomes
//
//   u'(i,j) = 1/2 u(i,j) + 1/8 ( u(i-1,j) + u(i+1,j) + u(i,j-1) + u(i,j+1) ).
//
// The weights are non-negative and sum to 1, which is the point of this form
// and not the more usual u + alpha (sum of neighbours - 4 u): a sweep cannot
// make any cell larger in magnitude than the largest cell of the grid, so
// every value in every sweep stays within [-M0, M0], M0 = max |u0|, and the
// rounding error of the tolerance below is a number, not a growth factor. A
// kernel that evaluates the other form is not this specification.
// The five terms u/2, N/8, S/8, E/8, W/8 are each exactly representable
// (multiplying a double by a power of two is exact, there is no underflow in
// the range of a correct input), and a kernel adds them in any order and any
// association, with or without fused multiply-add (an intrinsic, never
// contraction: -ffp-contract=off): the bound below covers every one of them.
//
// DECOMPOSITION: the tile edge, one of kTileEdges (cells), default
// kDefaultTileEdge; each divides n. How a sweep is tiled, and in which order a
// cell's five terms are added -- which a kernel may let depend on where the
// cell sits in its tile (a lane position, an edge cell done by scalar code) --
// are the kernel's. So bits may differ between tile edges, though the
// specification does not require that they do; if a stream finds its kernels
// independent of the tile edge, it records that under "Contract questions" and
// the class is revisited by contract change, never weakened or strengthened
// inside the stream.
//
// DETERMINISM: kBitExactFixedDecomposition. The same bits for one (tile edge,
// ISA path) at any thread count and on any run; across tile edges or across
// ISA paths, within the derived tolerance only (determinism.md §3). The
// scalar path additionally gives the same bits on x86-64 and ARM64. The
// oracle's contract is Contract::kBounded.
//
// THE CHECK, AND ITS BOUND
// ------------------------
// The oracle updates kRegions windows of kRegionEdge x kRegionEdge cells by
// plain scalar code, from the INPUT it is given, and compares them with the
// output it is given. To obtain a window's cells after kSteps sweeps it
// sweeps the window grown by kSteps cells on every side, clipped to the grid
// (a cell after sweep s depends only on cells within s of it), shrinking the
// region by one cell per side per sweep and holding the ring fixed.
//   * Window 0 has its origin (its first row and column) at (0, 0), and
//     window 1 at (n - kRegionEdge, n - kRegionEdge): the two corners, where
//     the ring and the clipping are exercised. Windows 2 and 3 have origins,
//     both coordinates in [0, n - kRegionEdge], that are a function of the
//     unit index alone, so a re-run checks the same cells. Windows may
//     overlap; a cell in two windows is compared once per window.
//   * The cells are compared window by window (0, 1, 2, 3), each in row-major
//     order. A cell passes when residual = |u^ - u'| <= T', with the
//     subtraction rounded in double.
//
// The verdict, in the terms of oracle.hpp:
//   * Agreement{kRegions * kRegionEdge^2} when every compared cell passes;
//   * BoundedMismatch for the FIRST cell, in the order above, that fails:
//     check "stencil-cell", element = i * n + j, residual its residual,
//     tolerance T', blocks_compared the number of cells compared up to and
//     including it. Of several failing cells, therefore, the first in that
//     order is the one named. A NaN or infinity in the output cell makes the
//     residual NaN or infinite, which fails (`residual <= T'` is false for NaN);
//   * NotChecked (blocks_compared 0) when the evidence is not of the sizes
//     kGridBytes; or when any input cell is not finite or has magnitude above
//     1 (the tolerance cannot be derived; the input-integrity bracket is what
//     reports the corrupt input itself); or when every input cell is zero
//     (T would be 0, and a verdict's tolerance is positive). A NaN or
//     infinity in the OUTPUT is not this: it is a BoundedMismatch.
//
// Notation. u = kUnitRoundoff = 2^-53; gamma_k = k u / (1 - k u);
// T = kSteps; M0 = the largest |cell| of the input grid (exact: a maximum
// rounds nothing), over the WHOLE grid, not only the window.
//
// Derivation of T_cell.
//   One sweep, per interior cell: the computed value is the sum of five
//   exactly represented terms by at most four additions (or fused
//   multiply-adds whose products are exact), each term passing through at
//   most four roundings, so (Higham, Accuracy and Stability of Numerical
//   Algorithms, 2nd ed., section 4.2) the computed u^' satisfies
//     |u^'(i,j) - (A u^)(i,j)| <= gamma_4 sum |terms| <= gamma_4 ||u^||_inf,
//   where A is the exact sweep and the weights sum to 1. Ring cells are
//   copied, and have no error. Let e_s = ||u^_s - u_s||_inf, u_s the exact
//   grid after s sweeps and u^_s the computed one, e_0 = 0 (the input is
//   exact). ||A||_inf = 1 (non-negative rows summing to 1), and ||u_s||_inf <=
//   M0 (the maximum principle, same reason), so
//     e_{s+1} <= ||A||_inf e_s + gamma_4 (||u_s||_inf + e_s) <= (1 + gamma_4) e_s + gamma_4 M0,
//   and by induction e_s <= M0 ((1 + gamma_4)^s - 1). With 1 + gamma_4 =
//   1/(1 - 4u) and (1 - 4u)^s >= 1 - 4 s u (Bernoulli),
//     e_T <= M0 gamma_{4T}.
//   The oracle's windows follow the same recursion on the cells they compute
//   (a cell's error depends only on cells within its sweep count, all of which
//   the oracle computes), so its error is also at most M0 gamma_{4T}. Hence
//     |u^ - u'| <= 2 gamma_{4T} M0 =: T_cell = cell_tolerance_factor() * M0.
//   The tolerance of a verdict is
//     T' = (evaluated T_cell) (1 + kEvaluationSlack),   kEvaluationSlack = 2^-36.
//   gamma_{4T} is evaluated with at most two roundings, so the evaluated
//   T_cell is within a relative 2^-37 of the real number; with the one
//   rounding of the subtraction that makes the residual, an output whose
//   exact error is within T_cell is never rejected, and one whose exact
//   error exceeds T_cell (1 + 2^-34) is always rejected. The oracle stream
//   tests the bound breached at (1 + epsilon) times T' and held at
//   (1 - epsilon) times it, epsilon = 2^-20.
//
// What the bound does not claim: an error below T_cell (about 7e-15 for a grid
// whose largest cell is 1, i.e. a flip below roughly the 48th mantissa bit of
// a cell of magnitude 1) goes unseen, and cells outside the four windows are
// not examined; the ring is checked only where a window covers it, so the
// requirement that the output ring equals the input ring bit for bit is
// tested by the workload's own tests.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::stencil {

/// The name configuration and reports use: Descriptor::name.
inline constexpr std::string_view kName = "stencil";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kEdge = 256;
inline constexpr std::size_t kSteps = 8;
inline constexpr std::size_t kGridBytes = kEdge * kEdge * sizeof(double);
inline constexpr std::size_t kInputBytes = kGridBytes;
inline constexpr std::size_t kOutputBytes = kGridBytes;

inline constexpr std::array<std::uint32_t, 4> kTileEdges = {16, 32, 64, 128};
inline constexpr std::uint32_t kDefaultTileEdge = 64;

/// The oracle's sampled windows.
inline constexpr std::size_t kRegions = 4;
inline constexpr std::size_t kRegionEdge = 16;
inline constexpr std::size_t kCellsChecked = kRegions * kRegionEdge * kRegionEdge;

/// The entry a 64-bit draw makes: 2 * (bits >> 11) * 2^-53 - 1, a multiple of
/// 2^-52 in [-1, 1). Exact: no rounding happens anywhere in it.
[[nodiscard]] constexpr double entry_value(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus52 = 1.0 / 4503599627370496.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus52 - 1.0;
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_tile_edge(std::uint32_t edge) noexcept {
  return std::ranges::any_of(kTileEdges, [edge](std::uint32_t tile) { return tile == edge; });
}

/// Whether cell (row, column) is on the fixed ring.
[[nodiscard]] constexpr bool is_ring(std::size_t row, std::size_t column) noexcept {
  return row == 0 || column == 0 || row == kEdge - 1 || column == kEdge - 1;
}

/// u = 2^-53 and gamma_k = k u / (1 - k u), the constants of the bound.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
[[nodiscard]] constexpr double gamma(std::size_t k) noexcept {
  const double ku = static_cast<double>(k) * kUnitRoundoff;
  return ku / (1.0 - ku);
}

/// The relative allowance every tolerance carries, 2^-36.
inline constexpr double kEvaluationSlack = 1.0 / 68719476736.0;

/// 2 gamma_{4 kSteps}: T_cell is this times M0, the largest |cell| of the input.
[[nodiscard]] constexpr double cell_tolerance_factor() noexcept { return 2.0 * gamma(4 * kSteps); }

}  // namespace loadforge::workloads::stencil

#endif
