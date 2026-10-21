// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_FFT_SPEC_HPP
#define LOADFORGE_WORKLOADS_FFT_SPEC_HPP

// CONTRACT (ADR 0004). The specification the FFT workload and its independent
// oracle are both written from (verification.md §3, §5; determinism.md §3, §4;
// IMPLEMENTATION M4). Frozen: changed only by a contract change, never inside
// a stream.
//
//   The FFT stream writes the stress implementation: a multi-threaded radix-r
//     FFT with scalar, AVX2 (FMA3) and NEON kernels behind the dispatch seam
//     (src/workloads/fft/).
//   The wave-3 oracle stream writes the oracle (a direct O(n^2) DFT on sampled
//     bins, and Parseval, src/verification/oracle/fft/) and the derived
//     tolerances, from this file and nothing else -- never from the kernels.
//
// VERSIONS. kVersion versions this SPECIFICATION: the unit's format and the
// check below. A change to either increments it. The workload's own bits are
// versioned separately, by Descriptor::version (workload.hpp), which moves
// whenever a configuration's output changes -- including when this
// specification does not.
//
// A UNIT: THE FORWARD DFT OF n = kPoints COMPLEX POINTS
// -----------------------------------------------------
// Write x_j = (re_j, im_j) for point j in [0, n), and
//
//   X_k = sum_{j=0}^{n-1} x_j w^{jk},   w = exp(-2 pi i / n),   k in [0, n):
//
// the forward transform, unnormalised (no factor of 1/n anywhere).
//
//   input   2 n little-endian IEEE 754 doubles, re_0, im_0, re_1, im_1, ...
//           Element e of the unit (e in [0, 2 n); element 2 j is re_j and
//           element 2 j + 1 is im_j) is
//             entry_value(u64_of(block_at(seed, index, e)))
//           (primitives/rng/philox.hpp): a multiple of 2^-52 in [-1, 1 - 2^-52].
//           The lower end IS reached (entry_value(0) == -1.0); the upper end,
//           +1, is not. Every correctly generated component is finite with
//           magnitude at most 1 -- the tolerances below rely on it. Only the
//           first two words of each Philox block are used.
//   output  X in natural order k = 0 .. n-1, in the layout of the input: 2 n
//           little-endian doubles, Re X_0, Im X_0, Re X_1, Im X_1, ...
//
// THE TWIDDLE TABLE. libm's cos and sin are not reproducible across machines
// (determinism.md §4), so the workload never calls them. W[m], for m in
// [0, n), is the complex number
//
//   Re W[m] = RN( cos(2 pi m / n) ),   Im W[m] = RN( -sin(2 pi m / n) ),
//
// where RN(a) is the double nearest the REAL number a. There are no ties
// (every a that is not exactly 0, 1 or -1 is irrational), and an exact zero
// is +0.0, never -0.0. This definition is unique, so two independently built
// tables are equal bit for bit. It is committed to the repository as a table
// generated offline with arbitrary-precision arithmetic, never at run time.
// Three entries are where a libm table goes wrong, and are pinned:
//   W[0]     = (1, +0);
//   W[n/4]   = (+0, -1)     (cos(pi/2) is exactly 0 here, not 6.1e-17);
//   W[n/8]   = (kSqrtHalf, -kSqrtHalf),  kSqrtHalf the double nearest sqrt(1/2),
//              whose bits are kSqrtHalfBits.
// Every twiddle factor the workload multiplies by is W[m] for some m, or one
// of W[m]'s exact images: W[m + n/2] = -W[m], W[m + n/4] = -i W[m] (swap the
// parts, negate one), and W[n/4 - m] = (-Im W[m], -Re W[m]). Nothing is
// computed at run time. Each part of a twiddle is therefore within a
// relative u of the true value, so
//   |W[m] - w^m| <= u,   u = kUnitRoundoff,
// and |W[m]| <= 1 + u.
//
// DECOMPOSITION: the radix r, one of kRadices, default kDefaultRadix. The
// transform is t = stage_count(r) stages of radix r (n = r^t; both radices
// divide n = 4096 = 2^12 = 4^6). Each stage is the same operation on r
// elements at a time: multiply the butterfly's inputs by twiddles from the
// table (the first input of a butterfly by 1, so by nothing), then form its r
// outputs as the r-point DFT of the products -- sums and differences with
// the factors 1, -1, i, -i, which for r = 2 and r = 4 are swaps of parts and
// sign changes, never a rounded constant. Every stage's butterflies together
// take each element of the stage's input exactly once. Digit-reversal
// permutations are exact data movement. Within that model the evaluation
// order of a butterfly's sums, the choice of decimation in time or Stockham
// autosort formulation, and the use of fused multiply-add where the kernel
// names it (an intrinsic, never contraction: -ffp-contract=off) are the
// kernel's. Radix 8 or 16, split-radix and any algorithm whose stage is not
// of this form is outside the specification; the tolerance below does not
// cover it.
//
// DETERMINISM: kBitExactFixedDecomposition. The same bits for one (radix, ISA
// path) at any thread count and on any run; across radices or across ISA
// paths, within the derived tolerance only (determinism.md §3). The scalar
// path additionally gives the same bits on x86-64 and ARM64: the table is
// bit-identical and nothing else is a transcendental. The oracle's contract
// is Contract::kBounded.
//
// THE CHECK, AND ITS BOUNDS
// -------------------------
// The oracle makes kSampledBins + 1 checks, in this order, and stops at the
// first that fails:
//   checks 1 .. kSampledBins   a direct DFT of the input at one bin each, the
//                              sampled bins in increasing k;
//   check kSampledBins + 1     Parseval over all n bins.
// The sampled bins are kSampledBins distinct values of k, always including
// k = 0 and k = n/2 (where the twiddles are +-1), the others a function of
// the unit index alone, so a re-run checks the same ones. A round-trip
// (inverse transform) is not checked: it needs a second FFT, or a direct
// inverse at n^2 operations per bin, and Parseval already covers every output
// bin at O(n); the round trip is a test of the workload's own (stream 22).
//
// The verdict, in the terms of oracle.hpp:
//   * Agreement{kSampledBins + 1} when every check holds;
//   * BoundedMismatch for the first check that fails, blocks_compared its
//     1-based ordinal in the order above (so the number of checks made,
//     including the failing one). Its `check` names it: "dft-bin" for 1 ..
//     kSampledBins, with element = k and residual = max(|a|, |b|) for
//     a = Re X^_k - Re X'_k and b = Im X^_k - Im X'_k (the subtractions
//     rounded in double); "parseval" for the last, with no element and
//     residual |s^ - 64 a^| (below). tolerance is T' below. A NaN or
//     infinity in a compared output value makes the residual NaN or
//     infinite, which fails (a comparison `residual <= T'` is false for NaN);
//   * NotChecked (blocks_compared 0) when the evidence is not of the sizes
//     kInputBytes and kOutputBytes; or when any input component is not finite
//     or has magnitude above 1 (the tolerance cannot be derived; the
//     input-integrity bracket is what reports the corrupt input itself); or
//     when every input component is zero (T would be 0, and a verdict's
//     tolerance is positive). A NaN or infinity in the OUTPUT is not this: it
//     is a BoundedMismatch.
// Of several failing bins, the one with the smallest k is named, because the
// bins are checked in increasing k; a failing bin takes precedence over
// Parseval, which is only reached when every bin held.
//
// Notation. u = kUnitRoundoff = 2^-53; gamma_k = k u / (1 - k u); n = kPoints;
// X^ is the workload's output; X the exact DFT of the input x; the 2-norm of a
// complex vector is sqrt(sum |.|^2).
//
// 1. The error of the workload's transform, in the 2-norm.
//   One stage, applied to a vector z of n complex numbers computed so far:
//   exactly, y = A z with A = B D, D the diagonal of exact twiddles w^m and B
//   the r-point butterflies. D is unitary and B is sqrt(r) times a unitary
//   matrix, so ||y|| = sqrt(r) ||z||. Computed with the table, y^ =
//   fl(B D^ z), D^ the diagonal of table values. Two kinds of error:
//     (a) D^ against D: ||B (D^ - D) z|| <= sqrt(r) u ||z||, since each
//         diagonal entry differs by at most u.
//     (b) rounding. Each real part of each output of a butterfly is a signed
//         sum of at most 2 r real products (two per complex product, r
//         products' worth of them), formed by additions, subtractions and
//         multiplications, with or without fused multiply-add, in any order
//         and any nesting. Every product passes through at most one
//         multiplication rounding and at most 2 r - 1 additions, so the
//         standard bound (Higham, Accuracy and Stability of Numerical
//         Algorithms, 2nd ed., sections 3.1 and 4.2: |fl(sum) - sum| <=
//         gamma_m sum |terms|, m the number of roundings a term passes
//         through) gives for each real component of each output
//           |error| <= gamma_{2r} sum_inputs |D^_m| |z_m| <= gamma_{2r}(1+u) sum |z_m|,
//         and for the complex output at most sqrt(2) times that. The r inputs
//         of a butterfly have sum |z_m| <= sqrt(r) ||z_butterfly|| (Cauchy-
//         Schwarz), and the butterfly has r outputs, so the 2-norm of its
//         error is at most sqrt(2) r gamma_{2r}(1+u) ||z_butterfly||.
//         The butterflies of a stage partition the vector, so over the stage
//         ||error|| <= sqrt(2) r gamma_{2r}(1+u) ||z||.
//   Together: ||y^ - A z|| <= eta_r sqrt(r) ||z||, with
//     eta_r = u + sqrt(2 r) gamma_{2r}(1+u) <= u + r gamma_{2r}(1+u)
//   (sqrt(2 r) <= r for r >= 2). That is stage_error(r).
//   Over t stages: let z_s be the exact result after stage s and z^_s the
//   computed one, z_0 = z^_0 = x (the input is exact), rho_s = ||z^_s - z_s|| /
//   ||z_s||, ||z_s|| = r^{s/2} ||x||. Then
//     z^_s - z_s = (z^_s - A_s z^_{s-1}) + A_s (z^_{s-1} - z_{s-1}),
//   whose first term has norm at most eta_r sqrt(r) ||z^_{s-1}|| <= eta_r
//   sqrt(r) (1 + rho_{s-1}) ||z_{s-1}|| and whose second has norm sqrt(r)
//   rho_{s-1} ||z_{s-1}||. Dividing by ||z_s|| = sqrt(r) ||z_{s-1}||:
//     rho_s <= eta_r (1 + rho_{s-1}) + rho_{s-1},  so
//     rho_t <= (1 + eta_r)^t - 1 <= t eta_r / (1 - t eta_r) =: rho
//   ((1+eta)^t <= e^{t eta} <= 1/(1 - t eta) for t eta < 1). That is
//   forward_error_factor(r). With ||X|| = sqrt(n) ||x|| (Parseval, exact):
//     ||X^ - X|| <= rho sqrt(n) ||x||,  and so  |X^_k - X_k| <= rho sqrt(n) ||x||
//   for every bin (a component of a vector is no larger than its norm).
//   Numerically rho is about 1.2e-14 (r = 2) and 1.6e-14 (r = 4).
//
// 2. The error of the oracle's own DFT at a bin. X'_k = sum_j x_j W[jk mod n],
//   formed by plain scalar loops (std::fma permitted, any fixed order; the
//   index jk mod n is exact integer arithmetic). Against the exact X_k:
//   the table contributes |sum x_j (W - w^{jk})| <= u S, S = sum_j |x_j|; the
//   rounding contributes, per real component, a signed sum of 2 n products,
//   at most gamma_{2n} (1+u) S, and sqrt(2) times that for the complex value.
//   So |X'_k - X_k| <= (u + sqrt(2) gamma_{2n}(1+u)) S, and S <= sqrt(n) ||x||
//   (Cauchy-Schwarz).
//
// 3. The bin check. |X^_k - X'_k| <= |X^_k - X_k| + |X_k - X'_k|, hence
//     T_bin = sqrt(n) ||x|| (rho + u + sqrt(2) gamma_{2n}(1+u)).
//   Each of |a| and |b| (the real and imaginary parts of the difference) is
//   at most the modulus, so each is within T_bin. That is
//   bin_tolerance_factor(r) times sqrt(n) ||x||; sqrt(2) is taken as the
//   constant kSqrtTwoBound = 3/2 >= sqrt(2).
//
// 4. The Parseval check. Let s^ = fl(sqrt(fl(sum of the 2 n squares of X^'s
//   parts))) and a^ the same for x. A sum of 2 n squares has relative error
//   at most gamma_{2n}, the square root of 1+theta differs from 1 by at most
//   |theta|, and the root rounds once more: |s^ - ||X^||| <= gamma_{2n+1}
//   ||X^|| and |a^ - ||x||| <= gamma_{2n+1} ||x||. sqrt(n) = 64 exactly, so
//   64 a^ is exact. Then
//     |s^ - 64 a^| <= | ||X^|| - ||X|| | + gamma_{2n+1} ||X^|| + 64 gamma_{2n+1} ||x||
//                  <= 64 ||x|| (rho + gamma_{2n+1} (2 + rho)) =: T_parseval,
//   using | ||X^|| - ||X|| | <= ||X^ - X|| <= rho ||X|| and ||X^|| <= (1+rho)
//   ||X||. That is parseval_tolerance_factor(r) times 64 ||x||.
//
// 5. Evaluation. The oracle cannot know ||x|| exactly: it uses a^ for it
//   (the two differ by a relative gamma_{2n+1} < 2^-39). Every factor above is
//   a product of a few doubles or a sum of at most 2 n + 1 positive terms, so
//   the tolerance as evaluated in double differs from the real number above
//   by a relative 2^-37 at most. The tolerance of a verdict is
//     T' = (evaluated T) (1 + kEvaluationSlack),   kEvaluationSlack = 2^-36,
//   and a check fails when residual <= T' is false. Consequences: an output
//   whose exact error is within T is never rejected (the extra relative
//   2^-36 exceeds every evaluation error and the one rounding of the
//   subtraction), and one whose exact error exceeds T (1 + 2^-34) is always
//   rejected. The oracle stream tests the bound breached at (1 + epsilon)
//   times T' and held at (1 - epsilon) times it, for epsilon = 2^-20, for
//   both radices and both checks.
//
// What the bound does not claim: an error below T goes unseen, so the
// sampled bins and Parseval together catch large corruption of the output
// (an exponent or high-mantissa bit flip in any sampled bin; an error in any
// bin comparable to T_parseval) and not the low bits of any one value. That is
// the price of a check that does not trust the stress implementation, and
// the same price dense-linear pays.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::fft {

/// The name configuration and reports use: Descriptor::name.
inline constexpr std::string_view kName = "fft";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kPoints = 4096;
/// A unit's input and its output are each 2 * kPoints doubles.
inline constexpr std::size_t kInputBytes = 2 * kPoints * sizeof(double);
inline constexpr std::size_t kOutputBytes = 2 * kPoints * sizeof(double);

inline constexpr std::array<std::uint32_t, 2> kRadices = {2, 4};
inline constexpr std::uint32_t kDefaultRadix = 4;

/// The bins the oracle's direct DFT checks, and the Parseval check after them.
inline constexpr std::size_t kSampledBins = 16;
inline constexpr std::size_t kChecks = kSampledBins + 1;

/// The double nearest sqrt(1/2) = 0.7071067811865475244..., and its bits:
/// W[n/8] = (kSqrtHalf, -kSqrtHalf).
inline constexpr double kSqrtHalf = 0.70710678118654757;
inline constexpr std::uint64_t kSqrtHalfBits = 0x3FE6A09E667F3BCDULL;

/// The entry a 64-bit draw makes: 2 * (bits >> 11) * 2^-53 - 1, a multiple of
/// 2^-52 in [-1, 1). Exact: no rounding happens anywhere in it.
[[nodiscard]] constexpr double entry_value(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus52 = 1.0 / 4503599627370496.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus52 - 1.0;
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_radix(std::uint32_t radix) noexcept {
  return std::ranges::any_of(kRadices, [radix](std::uint32_t r) { return r == radix; });
}

/// t, the number of stages: the least t with radix^t >= kPoints. For a radix
/// of kRadices, radix^t == kPoints. Precondition: radix >= 2.
[[nodiscard]] constexpr std::uint32_t stage_count(std::uint32_t radix) noexcept {
  std::uint64_t reach = 1;
  std::uint32_t stages = 0;
  while (reach < kPoints) {
    reach *= radix;
    ++stages;
  }
  return stages;
}

/// u = 2^-53 and gamma_k = k u / (1 - k u), the constants of the bounds.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
[[nodiscard]] constexpr double gamma(std::size_t k) noexcept {
  const double ku = static_cast<double>(k) * kUnitRoundoff;
  return ku / (1.0 - ku);
}

/// A bound on sqrt(2) the derivation uses where the complex modulus of an error
/// is taken from its two parts.
inline constexpr double kSqrtTwoBound = 1.5;

/// The relative allowance every tolerance carries, 2^-36 (derivation, 5).
inline constexpr double kEvaluationSlack = 1.0 / 68719476736.0;

/// eta_r = u + r gamma_{2r} (1 + u): the relative error one stage adds, for
/// radix r (derivation, 1). Precondition: radix >= 2.
[[nodiscard]] constexpr double stage_error(std::uint32_t radix) noexcept {
  return kUnitRoundoff +
         static_cast<double>(radix) * gamma(std::size_t{2} * radix) * (1.0 + kUnitRoundoff);
}

/// rho = t eta_r / (1 - t eta_r): the bound on ||X^ - X|| / ||X|| (derivation,
/// 1). Precondition: is_radix(radix).
[[nodiscard]] constexpr double forward_error_factor(std::uint32_t radix) noexcept {
  const double t_eta = static_cast<double>(stage_count(radix)) * stage_error(radix);
  return t_eta / (1.0 - t_eta);
}

/// The factor T_bin carries beyond sqrt(n) ||x||: rho + u + sqrt(2)
/// gamma_{2n} (1 + u) (derivation, 3). Precondition: is_radix(radix).
[[nodiscard]] constexpr double bin_tolerance_factor(std::uint32_t radix) noexcept {
  return forward_error_factor(radix) + kUnitRoundoff +
         kSqrtTwoBound * gamma(2 * kPoints) * (1.0 + kUnitRoundoff);
}

/// The factor T_parseval carries beyond 64 ||x||: rho + gamma_{2n+1} (2 + rho)
/// (derivation, 4). Precondition: is_radix(radix).
[[nodiscard]] constexpr double parseval_tolerance_factor(std::uint32_t radix) noexcept {
  const double rho = forward_error_factor(radix);
  return rho + gamma(2 * kPoints + 1) * (2.0 + rho);
}

}  // namespace loadforge::workloads::fft

#endif
