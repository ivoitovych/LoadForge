// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_DENSE_LINEAR_SPEC_HPP
#define LOADFORGE_WORKLOADS_DENSE_LINEAR_SPEC_HPP

// CONTRACT (ADR 0004). The specification the dense-linear workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md; IMPLEMENTATION M2b). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The dense-linear stream writes the stress implementation: a blocked,
//     vectorised matrix product with scalar, AVX2 (FMA3) and NEON kernels
//     behind the dispatch seam (src/workloads/dense_linear/, kernels under
//     src/primitives/simd/x86/ and src/primitives/simd/arm/).
//   The bounded-verification stream writes the oracle (naive scalar dot
//     products on sampled entries, src/verification/oracle/dense_linear/) and
//     the derived tolerance (src/verification/residual/) -- never from the
//     kernels.
//
// A UNIT: C = A B, for n x n matrices of doubles, n = kOrder
// -----------------------------------------------------------
//   input   A then B, each row-major, n*n little-endian IEEE 754 doubles:
//           2 n^2 * 8 bytes. Element e of the unit (A's entries first, then
//           B's) is entry_value(u64_of(block_at(seed, index, e))): a multiple
//           of 2^-52 in [-1, 1). Every correctly generated entry is finite
//           with magnitude below 1 -- the tolerance derivation relies on it.
//   output  C, row-major, n*n little-endian doubles.
//
// DECOMPOSITION: the k-panel depth kc, one of kPanelDepths, default
// kDefaultPanelDepth. Each C entry accumulates panel by panel in increasing k;
// within a panel, the order is the kernel's for the ISA path. Determinism
// class: kBitExactFixedDecomposition -- the same bits for one (kc, ISA path)
// at any thread count and on any run; across kc or across paths, within the
// derived tolerance only (determinism.md §3). The scalar path additionally
// gives the same bits on x86-64 and ARM64. FMA is used where the kernel
// names it (an intrinsic), never by contraction (-ffp-contract=off).
//
// THE CHECK, AND ITS BOUND
// ------------------------
// For a sampled entry (i, j) the oracle computes c' = sum_k a_ik b_kj and
// s = sum_k |a_ik| |b_kj| by plain scalar loops (std::fma permitted, any fixed
// order), and requires |c_ij - c'| <= tolerance_ij, with tolerance_ij derived
// from the standard bound for a dot product in any order, with or without
// fused multiply-add: |fl(x.y) - x.y| <= gamma_n * sum |x_k||y_k|, gamma_n =
// n u / (1 - n u), u = 2^-53 (Higham, Accuracy and Stability of Numerical
// Algorithms, 2nd ed., section 3.1). Both the kernel's result and the oracle's
// are within gamma_n * s of the exact value, s itself is computed within a
// factor (1 + gamma_n), and the subtraction and comparison round once more:
// the oracle stream writes that derivation in full in its header, and tests
// the bound breached at (1 + epsilon) times it and held at (1 - epsilon)
// (M2b exit criterion 3). Sampling: which entries is a function of the unit
// index alone; blocks_compared counts entries checked.
//
//   * an entry outside its tolerance, or a NaN or infinity in C, is a
//     BoundedMismatch naming the entry (element = i * n + j);
//   * a non-finite or out-of-range entry in the INPUT means the tolerance
//     cannot be derived: NotChecked. The input-integrity bracket is what
//     reports the corrupt input itself.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace loadforge::workloads::dense_linear {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kOrder = 256;
inline constexpr std::array<std::uint32_t, 5> kPanelDepths = {16, 32, 64, 128, 256};
inline constexpr std::uint32_t kDefaultPanelDepth = 64;
inline constexpr std::size_t kInputBytes = 2 * kOrder * kOrder * sizeof(double);
inline constexpr std::size_t kOutputBytes = kOrder * kOrder * sizeof(double);

/// The entry a 64-bit draw makes: 2 * (bits >> 11) * 2^-53 - 1, a multiple of
/// 2^-52 in [-1, 1). Exact: no rounding happens anywhere in it.
[[nodiscard]] constexpr double entry_value(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus52 = 1.0 / 4503599627370496.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus52 - 1.0;
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_panel_depth(std::uint32_t kc) noexcept {
  return std::ranges::any_of(kPanelDepths, [kc](std::uint32_t depth) { return depth == kc; });
}

/// u = 2^-53 and gamma_n = n u / (1 - n u), the constants of the bound.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
[[nodiscard]] constexpr double gamma(std::size_t n) noexcept {
  const double nu = static_cast<double>(n) * kUnitRoundoff;
  return nu / (1.0 - nu);
}

}  // namespace loadforge::workloads::dense_linear

#endif
