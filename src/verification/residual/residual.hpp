// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RESIDUAL_RESIDUAL_HPP
#define LOADFORGE_VERIFICATION_RESIDUAL_RESIDUAL_HPP

// Derived tolerances (docs/verification.md §5): the bound a bounded check
// compares a computed result against, derived from the operation count and
// the magnitudes involved, never chosen. Used by the dense-linear oracle
// (verification/oracle/dense_linear/) and by the invariant checks
// (verification/invariant/); every later bounded oracle builds on it.
//
// THE QUESTION THE TOLERANCE ANSWERS
// ----------------------------------
// Two programs evaluated the same sum of terms: the stress kernel, in an order
// of its own (blocked, vectorised, with or without fused multiply-add), and
// the oracle, in a plain loop. Neither result is exact, and they legitimately
// differ in their low bits (docs/determinism.md §3). The tolerance is the
// largest |kernel - oracle| that a CORRECT kernel can produce on the input the
// oracle was given. Anything larger is not rounding: it is a wrong result. The
// bound must never be exceeded by a correct computation (a false alarm would
// accuse healthy hardware, the failure this project exists to avoid), and it
// should be no looser than the rounding itself allows (a loose bound hides
// real corruption).
//
// THE DERIVATION
// --------------
// Floating-point model, with gradual underflow (Higham, Accuracy and Stability
// of Numerical Algorithms, 2nd ed., §2.1 and (2.8)): for +, -, *, and a fused
// multiply-add,
//
//     fl(x op y) = (x op y)(1 + d) + e,   |d| <= u = 2^-53,
//                                         |e| <= e_min = 2^-1075,  d e = 0,
//
// and e = 0 for addition and subtraction: a sum or difference that lands in
// the subnormal range is exact. (Round to nearest, no flush to zero: the
// floating-point environment contract, platform/fpenv/fpenv.hpp.)
//
// Step 1, the evaluation. A sum of t terms, each a product x_i y_i (a dot
// product) or a value x_i (a plain sum), is evaluated in ANY order: any
// binary tree of additions, each product formed by a multiplication or inside
// a fused multiply-add. Follow one term from the leaf to the root. It meets
// at most `depth` roundings, each a factor (1 + d), so (Higham Lemma 3.1) it
// arrives multiplied by (1 + theta), |theta| <= gamma_depth = depth u / (1 -
// depth u). A dot product of n terms has depth n (one multiply or FMA, then up
// to n - 1 additions above it); a plain sum of n values has depth n - 1. The
// only roundings that can carry an underflow term e are those that form a
// product, one per term (an FMA's e belongs to the one product it absorbs),
// and each e then meets at most depth - 1 further factors, so the e's together
// contribute at most m e_min (1 + gamma) <= m 2^-1074 =: U, with m the number
// of products (n for a dot product, 0 for a sum). With S = sum |term_i| the
// exact sum of the magnitudes:
//
//     |computed - exact| <= gamma_depth S + U.                          (1)
//
// (1) holds for the kernel's value, whatever its order, and for the oracle's.
//
// Step 2, the magnitudes. The oracle evaluates S alongside its value, by the
// same loop, as S^ = sum |x_i||y_i| (or sum |x_i|). Its terms are those of (1)
// in magnitude, so (1) applies to it, and since S^ >= S - gamma S - U,
//
//     S <= (S^ + U) / (1 - gamma).                                      (2)
//
// Step 3, the difference. By (1) twice and (2), for a correct kernel value c
// and the oracle's value c',
//
//     |c - c'| <= 2 gamma S + 2 U <= G (S^ + U) + 2 U,  G = 2 gamma / (1 - gamma).
//
// Step 4, the residual. It is computed as r^ = |fl(c - c')|, which rounds
// once and cannot underflow inexactly, so r^ <= (1 + u)|c - c'|, and a correct
// kernel always gives
//
//     r^ <= B := (1 + u) (G S^ + (G + 2) U).                            (3)
//
// Step 5, the tolerance as computed. B itself must be evaluated in floating
// point, and an evaluation that rounded below B would false-alarm. tolerance()
// computes
//
//     g^ = fl(nu / (1 - nu)), nu = depth u   (nu and 1 - nu are exact)
//     K  = up8(fl(2 g^ / fl(1 - g^)))        (up8: eight steps of nextafter
//                                             towards +infinity, each a factor
//                                             of at least (1 + u), exactly)
//     V  = (4 m + 1) 2^-1074                 (exact)
//     T^ = fl(fl(K S^) + V)
//
// and T^ >= B: the three roundings of K cost at most (1 - u)^4 against G, so
// K >= G (1 - u)^4 (1 + u)^8; fl(K S^) >= K S^ (1 - u) - e_min (the product
// may underflow); the final addition loses at most a factor (1 - u). So
// T^ >= G S^ (1 - u)^6 (1 + u)^8 + (V - e_min)(1 - u), and (1 - u)^6 (1 + u)^8
// >= 1 + u, while (V - e_min)(1 - u) >= 4 m 2^-1074 (1 - u) >= 3 U (1 + u) >=
// (G + 2) U (1 + u) because G <= 1 in the domain below. Every term of (3) is
// covered.
//
// So: r^ <= T^ for every correct computation, and r^ > T^ (or r^ NaN) is a
// wrong result. T^ is about twice Higham's bound for one dot product, because
// both evaluations may err, in opposite directions; it exceeds B by at most a
// relative 24 u (four roundings and eight steps of at most 2 u each) and by
// V, which is below 2^-1040 for any m in the domain.
// T^ is never zero: V > 0, which is what NumericalVerificationFailure
// requires of a tolerance (verification/record.hpp).
//
// THE DOMAIN, AND WHAT HAPPENS OUTSIDE IT
// --------------------------------------
// depth and m at most kMaxDepth (2^26), so gamma <= 2^-26, G <= 1, and 4 m + 1
// is exact; S^ finite, non-negative and at most kMaxMagnitude (2^1000), so
// T^ is finite and, since every partial sum of either evaluation is bounded by
// S (1 + gamma) plus underflow, neither evaluation overflowed. Outside the
// domain the tolerance cannot be derived, and tolerance() says why: a check
// without a derived tolerance is VerificationNotPerformed (verification.md
// §5), never a pass and never a guessed bound.
//
// The domain says nothing about the input's own range. A workload whose
// specification bounds its input (dense_linear/spec.hpp: |entry| < 1) checks
// that bound itself, because a value outside it is a corrupt input and the
// input-integrity bracket's finding, not the oracle's.
//
// DETERMINISM: pure functions of their arguments; the oracle's loops run in
// increasing index, multiply then add, with -ffp-contract=off (the build's
// rule), so the same input gives the same bits on every run, thread count and
// architecture. THREAD-SAFETY: pure functions.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/result.hpp"

namespace loadforge::verification::residual {

/// u = 2^-53, the unit roundoff of IEEE 754 binary64 under round to nearest.
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
/// The largest depth and the largest number of products the derivation holds
/// for (see the header comment).
inline constexpr std::size_t kMaxDepth = std::size_t{1} << 26U;
/// The largest magnitude sum the derivation holds for: 2^1000.
inline constexpr double kMaxMagnitude = 0x1p1000;

/// A value evaluated by the oracle, with the sum of the magnitudes of its
/// terms evaluated by the same loop: S^ of the derivation.
struct Accumulation {
  double value = 0.0;
  double magnitude = 0.0;
};

/// sum x_i y_i and sum |x_i||y_i|, in increasing i, each product formed and
/// then added: the oracle's dot product. Only the first min(x, y) elements of
/// each are used; callers pass spans of equal length.
[[nodiscard]] Accumulation dot(std::span<const double> x, std::span<const double> y) noexcept;

/// sum x_i and sum |x_i|, in increasing i.
[[nodiscard]] Accumulation sum(std::span<const double> x) noexcept;

/// gamma_k = k u / (1 - k u), evaluated as the derivation evaluates it.
/// Meaningful for k <= kMaxDepth.
[[nodiscard]] double gamma(std::size_t depth) noexcept;

/// What the derivation is told about an evaluation: the depth of the longest
/// rounding chain any term meets, and how many roundings form a product and so
/// may underflow (see the header comment, step 1).
struct ErrorModel {
  std::size_t depth = 0;
  std::size_t products = 0;
};

/// The model of a dot product of n terms, and of a plain sum of n values.
[[nodiscard]] constexpr ErrorModel dot_model(std::size_t n) noexcept { return {n, n}; }
[[nodiscard]] constexpr ErrorModel sum_model(std::size_t n) noexcept {
  return {n == 0 ? 0 : n - 1, 0};
}

/// Why a tolerance could not be derived.
///
///   kNonFiniteMagnitude  S^ is NaN or infinite: some term overflowed or was
///                        not a number.
///   kNegativeMagnitude   S^ < 0, which no sum of magnitudes is.
///   kMagnitudeTooLarge   S^ > kMaxMagnitude: the tolerance, or a partial sum
///                        of either evaluation, could overflow.
///   kDepthTooLarge       depth or products > kMaxDepth.
enum class Underivable : std::uint8_t {
  kNonFiniteMagnitude,
  kNegativeMagnitude,
  kMagnitudeTooLarge,
  kDepthTooLarge,
};

[[nodiscard]] std::string_view describe(Underivable why) noexcept;

/// T^ of the derivation: the largest |computed - reference| a correct
/// evaluation of the modelled kind can produce, given the oracle's magnitude
/// sum. Finite and strictly positive whenever it is derived.
[[nodiscard]] core::Result<double, Underivable> tolerance(ErrorModel model,
                                                          double magnitude) noexcept;

/// A computed result measured against the oracle's: r^ = |computed -
/// reference|, beside the tolerance it is judged by.
struct Comparison {
  double residual = 0.0;
  double tolerance = 0.0;
};

[[nodiscard]] Comparison compare(double computed, double reference, double tolerance) noexcept;

/// The comparison at the bound: the residual is no greater than the tolerance.
/// A NaN residual is never within it: a NaN is a failure, not a pass.
[[nodiscard]] bool within(const Comparison& comparison) noexcept;

}  // namespace loadforge::verification::residual

#endif
