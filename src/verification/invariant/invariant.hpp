// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_INVARIANT_INVARIANT_HPP
#define LOADFORGE_VERIFICATION_INVARIANT_INVARIANT_HPP

// Invariant and conservation checks (verification.md §3's pairings, §6): the
// properties a correct result keeps whatever order it was computed in, which
// later oracles combine with their scalar replays -- a sort's permutation and
// ordering checks (never a second sort), a transform's preserved norm, a
// simulation's conserved total, a reduction's claimed sum.
//
// TWO KINDS, AS THE TWO CONTRACTS (verification.md §5)
// ----------------------------------------------------
// The floating-point checks are BOUNDED: each returns the residual it measured
// and the tolerance verification/residual derives for it, or why no tolerance
// can be derived (a NaN or an infinity among the values, too many of them). A
// caller turns the first into an Agreement or a BoundedMismatch and the second
// into NotChecked; neither is ever a pass by default.
//
//   check_sum(claimed, values)
//       `claimed` is the sum of `values` as some other program computed it, in
//       any order. Residual |claimed - sum|, against the oracle's own sum in
//       increasing order; both are sums of n values, so the model is
//       residual::sum_model(n) and the derivation is residual's exactly.
//
//   check_squared_norm(claimed, values)
//       `claimed` is sum x_i^2 as another program computed it. Both sides are
//       dot products of x with itself: residual::dot_model(n), whose
//       underflow term covers squares of tiny values that round to subnormals.
//
//   check_conserved(before, after)
//       The premise, which is the caller's: in exact arithmetic the values of
//       `after` sum to the same total as those of `before` -- a permutation, a
//       redistribution, a transfer between cells. The oracle evaluates
//       sum(after) - sum(before) as ONE tree of n_a + n_b terms, whose exact
//       value is zero, so a correct result gives |that| <= gamma_d S, with
//       d = max(n_a, n_b) the deepest a term goes (its side's additions and the
//       final subtraction) and S the sum of all the magnitudes, evaluated by
//       the same tree. residual::tolerance({d, 0}, S^) bounds that, and is
//       twice as wide as this one evaluation needs, because it is derived for
//       two evaluations that may err in opposite directions; one derivation is
//       kept rather than a second, sharper one, at the cost of one bit of
//       sensitivity, which the fault-injection tests measure.
//
// The integer checks are EXACT: no tolerance, and a failure names what it can.
//
//   first_descent(values)
//       The first i with values[i] < values[i - 1], or nothing when the values
//       are in non-decreasing order: the element a sort put in the wrong place.
//
//   fingerprint(values), is_permutation(lhs, rhs)
//       A multiset fingerprint: the count, and two 64-bit sums, modulo 2^64,
//       of Philox4x32-10 applied to each value under a fixed key (the 128 bits
//       of one block). Addition is commutative, so the fingerprint ignores
//       order and nothing is sorted; Philox mixes every bit of a value into
//       every bit of its block, so a changed, dropped or duplicated value moves
//       both sums. Two different multisets of equal size agree only if 128
//       bits of pseudo-random sums collide -- about 2^-128 for corruption that
//       is not chosen against the key; the key is fixed so that a re-run gives
//       the same answer. It cannot say WHICH value was lost; first_descent
//       and the scalar replay say where.
//
// THREAD-SAFETY: pure functions of their arguments.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/result.hpp"
#include "verification/residual/residual.hpp"

namespace loadforge::verification::invariant {

/// The outcome of a bounded check: the measurement and its derived tolerance,
/// judged by residual::within(), or why no tolerance can be derived.
using Bounded = core::Result<residual::Comparison, residual::Underivable>;

[[nodiscard]] Bounded check_sum(double claimed, std::span<const double> values) noexcept;
[[nodiscard]] Bounded check_squared_norm(double claimed, std::span<const double> values) noexcept;
[[nodiscard]] Bounded check_conserved(std::span<const double> before,
                                      std::span<const double> after) noexcept;

[[nodiscard]] std::optional<std::size_t> first_descent(
    std::span<const std::uint64_t> values) noexcept;

/// The Philox seed the fingerprint hashes under. "PERMUTED" in ASCII.
inline constexpr std::uint64_t kFingerprintSeed = 0x5045524D55544544ULL;

struct Fingerprint {
  std::uint64_t count = 0;
  std::uint64_t low = 0;
  std::uint64_t high = 0;
};

/// Defined in invariant.cpp, not here: one definition, one coverage record
/// (journal §1.22).
[[nodiscard]] bool operator==(const Fingerprint& lhs, const Fingerprint& rhs) noexcept;

[[nodiscard]] Fingerprint fingerprint(std::span<const std::uint64_t> values) noexcept;

/// Whether `rhs` holds the same values as `lhs`, as a multiset, by fingerprint.
[[nodiscard]] bool is_permutation(std::span<const std::uint64_t> lhs,
                                  std::span<const std::uint64_t> rhs) noexcept;

}  // namespace loadforge::verification::invariant

#endif
