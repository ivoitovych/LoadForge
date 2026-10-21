// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_SPARSE_SPEC_HPP
#define LOADFORGE_WORKLOADS_SPARSE_SPEC_HPP

// CONTRACT (ADR 0004). The specification the sparse workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The sparse stream writes the stress implementation: a parallel conjugate-
//     gradient solver over a CSR matrix, with scalar, AVX2 (FMA3) and NEON
//     kernels behind the dispatch seam (src/workloads/sparse/).
//   The wave-3 oracle stream writes the oracle (a scalar sparse matrix-vector
//     product and the explicit residual, src/verification/oracle/sparse/) and
//     the derived tolerances, from this file and nothing else -- never from
//     the kernels.
//
// VERSIONS. kVersion versions this SPECIFICATION: the unit's format and the
// check below. A change to either increments it. The workload's own bits are
// versioned separately, by Descriptor::version (workload.hpp), which moves
// whenever a configuration's output changes -- including when this
// specification does not.
//
// A UNIT: A x = b FOR A SYMMETRIC POSITIVE-DEFINITE BANDED-SPARSE A, n = kRows
// ----------------------------------------------------------------------------
// The matrix has a fixed structure, the same for every unit and every seed;
// only its values and the right-hand side b come from the generator. Row i has
// entries at columns i - o_3, i - o_2, i - o_1, i, i + o_1, i + o_2, i + o_3
// for the offsets o_1 < o_2 < o_3 of kOffsets, keeping those in [0, n), in
// increasing column order -- so between 4 (rows 0 and n - 1: the diagonal and
// three on one side) and 7 entries, and kNonZeros in all. The structure is deliberately not a plain
// band: the offsets 97 and 1031 make a gather read three streams of x, which
// is what a sparse kernel's memory behaviour is about.
//
//   Values. For every pair (i, k), i in [0, n), k in [0, 3), with i + o_k <
//   n, the entries a(i, i + o_k) and a(i + o_k, i) are BOTH
//     entry_value(u64_of(block_at(seed, index, 3 i + k))) / 8
//   (a multiple of 2^-55 in [-1/8, 1/8): symmetric by construction; the
//   division is exact). Every diagonal entry is exactly kDiagonal = 2.0. The
//   right-hand side is b_i = entry_value(u64_of(block_at(seed, index, 3 n +
//   i))), a multiple of 2^-52 in [-1, 1 - 2^-52]. The lower ends ARE reached
//   (entry_value(0) == -1.0); the upper ends, +1 and +1/8, are not. Only the
//   first two words of each Philox block are used. Gershgorin's theorem puts
//   every eigenvalue of A in [2 - 6/8, 2 + 6/8] = [5/4, 11/4]: A is positive
//   definite with condition number at most 11/5.
//
//   input   little-endian, in this order, each part starting where the last
//           ended (the offsets are kValuesOffset .. kInputBytes):
//             values   kNonZeros doubles, row by row, in the column order
//                      above (CSR "values");
//             b        n doubles;
//             columns  kNonZeros u32, the column of each stored value (CSR
//                      "col_idx");
//             row_ptr  n + 1 u32: row_ptr[0] = 0, row_ptr[i + 1] - row_ptr[i]
//                      the number of entries of row i (CSR "row_ptr").
//   output  x^ (n doubles) then r^ (n doubles), kOutputBytes in all, where:
//             x^  the iterate after exactly kIterations steps of the
//                 conjugate-gradient recurrence below;
//             r^  the EXPLICIT residual of x^: r^_i = b_i - sum_j a_ij x^_j,
//                 formed by the workload with its own SpMV, one row after
//                 another as a signed sum of the row's m_i products and b_i,
//                 in any order, with or without fused multiply-add.
//
// THE ALGORITHM. Conjugate gradients without a preconditioner (the diagonal is
// constant, so a Jacobi preconditioner would only scale every vector by 1/2
// and change nothing), started at x = 0, for exactly kIterations iterations,
// with no convergence test, no early exit and no restart:
//   x_0 = 0,  r_0 = b,  p_0 = b,  rr_0 = r_0 . r_0;
//   for k = 0 .. kIterations - 1:
//     q = A p_k;  alpha = rr_k / (p_k . q);  x_{k+1} = x_k + alpha p_k;
//     r_{k+1} = r_k - alpha q;  rr_{k+1} = r_{k+1} . r_{k+1};
//     beta = rr_{k+1} / rr_k;  p_{k+1} = r_{k+1} + beta p_k.
//   x^ = x_kIterations.
// Division by zero, 0/0 and overflow are IEEE 754 arithmetic like any other
// and are not special-cased: a NaN spreads, and the workload still computes on
// every input byte pattern (workload.hpp: compute() is total). The generator's
// inputs never reach them (rr_k stays near 1e-28 or above at k = 15).
//
// DECOMPOSITION: the number of rows per chunk of a dot product, one of
// kChunkRows, default kDefaultChunkRows; each divides n into a power-of-two
// number of chunks. Every dot product (p . q, r . r) is formed as per-chunk
// partial sums -- each summed in the kernel's order for its ISA path -- and
// then the partials are combined by a tree whose shape is fixed by the
// specification and not by the thread count: adjacent pairs first, p_0 + p_1,
// p_2 + p_3, ..., then adjacent pairs of those sums, and so on to the root,
// each sum "left + right". The row dot products of the SpMV are the kernel's
// (their order within a row is its choice for the ISA path); the axpy updates
// are element-wise and have no order.
//
// DETERMINISM: kBitExactFixedDecomposition. The same bits for one (chunk rows,
// ISA path) at any thread count and on any run; across chunk sizes or across
// ISA paths, within the derived tolerance only (determinism.md §3). The scalar
// path additionally gives the same bits on x86-64 and ARM64. FMA is used where
// the kernel names it (an intrinsic), never by contraction
// (-ffp-contract=off). The oracle's contract is Contract::kBounded.
//
// THE CHECK, AND ITS BOUNDS
// -------------------------
// The oracle has the matrix, b and the output. It forms r~ = b - A x^ with a
// plain scalar SpMV (std::fma permitted, any fixed order), then makes n + 1
// checks in this order and stops at the first that fails:
//   checks 1 .. n   row i = 0, 1, ..., n - 1:  |r^_i - r~_i| <= T'_i, with
//                   T_i of derivation 1 below;
//   check n + 1     the residual norm: ||r~||_2 <= tau' (derivation 2), with
//                   ||.||_2 = fl(sqrt(fl(sum of squares))).
// The verdict, in the terms of oracle.hpp:
//   * Agreement{kRows + 1} when every check holds;
//   * BoundedMismatch for the first check that fails, blocks_compared its
//     1-based ordinal (the number of checks made, including the failing one).
//     Its `check` names it: "residual-row" for 1 .. n, with element = i and
//     residual |r^_i - r~_i| (rounded in double); "residual-norm" for the last,
//     with no element and residual ||r~||_2. tolerance is T'_i or tau'.
//     A NaN or infinity in x^ or r^ makes the affected residuals NaN or
//     infinite, which fail (`residual <= T'` is false for NaN); the first row
//     in which that happens is the one named;
//   * NotChecked (blocks_compared 0) when the evidence is not of the sizes
//     kInputBytes and kOutputBytes; or when the input is not what the
//     specification generates in a way the tolerances depend on: row_ptr or
//     columns differ from the fixed structure above, a value is not finite, a
//     diagonal entry is not exactly 2.0, an off-diagonal value has magnitude
//     above 1/8, a b_i is not finite or has magnitude above 1 (the tolerance
//     cannot be derived; the input-integrity bracket is what reports the
//     corrupt input itself), or every b_i is zero (tau would be 0 and a
//     verdict's tolerance is positive). It does NOT check that the matrix is
//     symmetric: a symmetric-looking corruption is the bracket's to find.
// Every tolerance T' or tau' is limited to [kMinTolerance, the largest finite
// double]: a row whose bound is 0 (b_i = 0 and every product zero) gets
// kMinTolerance, so that any nonzero r^_i fails; an x^ whose entries overflow
// the bound gets the largest double. (A verdict's tolerance is finite and
// positive.)
//
// Notation. u = kUnitRoundoff = 2^-53; gamma_k = k u / (1 - k u); m_i the
// number of entries of row i (4 to 7; the oracle reads it from row_ptr, which
// it has just checked).
//
// 1. The row checks. Exactly, r_i = b_i - sum_j a_ij x^_j; let s_i = |b_i| +
//   sum_j |a_ij| |x^_j|. The workload's r^_i and the oracle's r~_i are each a
//   signed sum of m_i products and b_i computed in some order, with or
//   without fused multiply-add. Each product passes through at most one
//   multiplication rounding and at most m_i additions, b_i through at most
//   m_i additions, so (Higham, Accuracy and Stability of Numerical
//   Algorithms, 2nd ed., sections 3.1 and 4.2)
//     |r^_i - r_i| <= gamma_{m_i+1} s_i  and  |r~_i - r_i| <= gamma_{m_i+1} s_i,
//   whatever x^ holds (it is an input to both). The oracle evaluates s~_i by
//   the same expression in any order; a sum of nonnegative terms, it has
//   relative error at most gamma_{m_i+1}, so s_i <= s~_i / (1 - gamma_{m_i+1}).
//   Hence
//     |r^_i - r~_i| <= 2 gamma_{m_i+1} s_i <= T_i := (2 gamma_{m_i+1} / (1 - gamma_{m_i+1})) s~_i.
//   That is row_tolerance_factor(m_i) * s~_i. This check is the one that sees
//   corruption of x^ after the solve, of the final SpMV, and of any bit of
//   x^ above the roundoff of a single product. It does not by itself say that
//   x^ solves the system -- that is check 2.
//
// 2. The residual norm. In exact arithmetic, conjugate gradients from x_0 = 0
//   satisfies ||e_K||_A <= 2 ((sqrt(kappa) - 1)/(sqrt(kappa) + 1))^K ||e_0||_A,
//   e = x - x* (Hestenes and Stiefel 1952; Saad, Iterative Methods for
//   Sparse Linear Systems, 2nd ed., Theorem 6.29), kappa = lambda_max /
//   lambda_min <= 11/5 by Gershgorin (above); sqrt(11/5) <= 3/2 and the rate
//   is increasing in kappa, so the rate is at most (3/2 - 1)/(3/2 + 1) = 1/5.
//   The residual is r_K = A e_K, so ||r_K||_2 = ||A^{1/2} A^{1/2} e_K||_2 <=
//   lambda_max^{1/2} ||e_K||_A, and ||e_0||_A = ||x*||_A = ||A^{-1/2} b||_2 <=
//   lambda_min^{-1/2} ||b||_2. Hence
//     ||r_K||_2 <= 2 sqrt(kappa) 5^-K ||b||_2 <= 3 * 5^-K ||b||_2.
//   With K = kIterations = 16 that is 1.97e-11 ||b||_2.
//   FINITE PRECISION IS THE ONE PLACE THIS SPECIFICATION DOES NOT PROVE. The
//   theorem is for exact arithmetic, and conjugate gradients in floating point
//   loses it in general; the rigorous finite-precision statements (Greenbaum
//   1989) are not closed-form. This unit is chosen so that the difference
//   cannot matter, and the specification takes the exact-arithmetic number
//   unchanged: the matrix is well conditioned, so the iteration follows the
//   exact one to a relative perturbation of order kappa n u (below 1e-11 on
//   the rate), and the attainable residual of any CG-type method on it is of
//   order u ||A||_2 ||x||_2 <= u (11/4)(4/5) ||b||_2 ~ 2.4e-16 ||b||_2
//   (||x||_2 <= ||b||_2 / lambda_min), while the bound above is 1.97e-11
//   ||b||_2: five orders higher. (Measured while writing this specification, a
//   scalar CG over four seeds ends at ||r||_2 / ||b||_2 of 2.2e-16, so the
//   bound is held by a factor of about 10^5.) The oracle checks
//     ||r~||_2 <= tau = 3 * 5^-K * ||b||_2   (residual_norm_factor() * ||b||_2),
//   r~ differing from the true residual by at most gamma_8 ||s||_2 and the norms
//   being evaluated with relative error at most gamma_{n+1} < 2^-38, both
//   negligible at that distance. This is an assumption about floating-point
//   CG, not a derivation, and is flagged as a contract question: the sparse
//   stream measures ||r^||_2 / ||b||_2 over many seeds and every ISA path and
//   records the largest, so that the bound can be tightened or the assumption
//   corrected by contract change.
//
// 3. Evaluation. Every tolerance as evaluated in double differs from its real
//   number by a relative 2^-37 at most (each is a product of a few doubles
//   and a sum of nonnegative terms). A verdict's tolerance is
//     T' = clamp((evaluated T) (1 + kEvaluationSlack), kMinTolerance, DBL_MAX),
//   kEvaluationSlack = 2^-36. So an output whose exact quantity is within T
//   is never rejected for evaluation error, and one whose exact quantity
//   exceeds T (1 + 2^-34) is always rejected (except where the clamp
//   intervenes). The oracle stream tests the bound breached at (1 + epsilon)
//   times T' and held at (1 - epsilon) times it, epsilon = 2^-20, for check 1
//   (at a row with m_i = 7 and at one with m_i = 4) and for check 2.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::sparse {

/// The name configuration and reports use: Descriptor::name.
inline constexpr std::string_view kName = "sparse";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kRows = 16384;
inline constexpr std::array<std::size_t, 3> kOffsets = {1, 97, 1031};
inline constexpr double kDiagonal = 2.0;
inline constexpr std::uint32_t kIterations = 16;

inline constexpr std::array<std::uint32_t, 4> kChunkRows = {256, 512, 1024, 2048};
inline constexpr std::uint32_t kDefaultChunkRows = 1024;

/// Entries of row `row`: the diagonal, and each of the six off-diagonal
/// positions row +- offset that lies in [0, kRows). Precondition: row < kRows.
[[nodiscard]] constexpr std::size_t row_length(std::size_t row) noexcept {
  std::size_t length = 1;
  for (const std::size_t offset : kOffsets) {
    length +=
        static_cast<std::size_t>(row >= offset) + static_cast<std::size_t>(row + offset < kRows);
  }
  return length;
}

/// The number of stored entries: the sum of row_length over every row, which
/// is kRows + 2 * sum over offsets of (kRows - offset).
inline constexpr std::size_t kNonZeros =
    kRows + 2 * ((kRows - kOffsets[0]) + (kRows - kOffsets[1]) + (kRows - kOffsets[2]));

/// Byte offsets of the parts of the input, and the sizes of input and output.
inline constexpr std::size_t kValuesOffset = 0;
inline constexpr std::size_t kRhsOffset = kValuesOffset + kNonZeros * sizeof(double);
inline constexpr std::size_t kColumnsOffset = kRhsOffset + kRows * sizeof(double);
inline constexpr std::size_t kRowPtrOffset = kColumnsOffset + kNonZeros * sizeof(std::uint32_t);
inline constexpr std::size_t kInputBytes = kRowPtrOffset + (kRows + 1) * sizeof(std::uint32_t);
inline constexpr std::size_t kOutputBytes = 2 * kRows * sizeof(double);

/// The entry a 64-bit draw makes: 2 * (bits >> 11) * 2^-53 - 1, a multiple of
/// 2^-52 in [-1, 1). Exact: no rounding happens anywhere in it.
[[nodiscard]] constexpr double entry_value(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus52 = 1.0 / 4503599627370496.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus52 - 1.0;
}

/// An off-diagonal value a 64-bit draw makes: entry_value / 8, a multiple of
/// 2^-55 in [-1/8, 1/8). Exact (a power-of-two scaling).
[[nodiscard]] constexpr double off_diagonal_value(std::uint64_t bits) noexcept {
  return entry_value(bits) * 0.125;
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_chunk_rows(std::uint32_t rows) noexcept {
  return std::ranges::any_of(kChunkRows, [rows](std::uint32_t chunk) { return chunk == rows; });
}

/// u = 2^-53 and gamma_k = k u / (1 - k u), the constants of the bounds.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
[[nodiscard]] constexpr double gamma(std::size_t k) noexcept {
  const double ku = static_cast<double>(k) * kUnitRoundoff;
  return ku / (1.0 - ku);
}

/// The relative allowance every tolerance carries, 2^-36.
inline constexpr double kEvaluationSlack = 1.0 / 68719476736.0;

/// The smallest tolerance a verdict carries: the smallest positive normal
/// double, 2^-1022.
inline constexpr double kMinTolerance = 2.2250738585072014e-308;

/// 2 gamma_{m+1} / (1 - gamma_{m+1}): T_i is this times s~_i (derivation 1),
/// for a row of m entries.
[[nodiscard]] constexpr double row_tolerance_factor(std::size_t entries) noexcept {
  const double g = gamma(entries + 1);
  return 2.0 * g / (1.0 - g);
}

/// 3 * 5^-kIterations: tau is this times ||b||_2 (derivation 2).
[[nodiscard]] constexpr double residual_norm_factor() noexcept {
  double power = 1.0;
  for (std::uint32_t k = 0; k < kIterations; ++k) {
    power *= 5.0;
  }
  return 3.0 / power;
}

}  // namespace loadforge::workloads::sparse

#endif
