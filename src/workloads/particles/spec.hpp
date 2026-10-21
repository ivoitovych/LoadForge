// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_SPEC_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_SPEC_HPP

// CONTRACT (ADR 0004). The specification the particles workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The particles stream writes the stress implementation: a cell-list
//     neighbour search and a vectorised short-range force sum, with scalar,
//     AVX2 (FMA3) and NEON kernels behind the dispatch seam
//     (src/workloads/particles/).
//   The wave-3 oracle stream writes the oracle (a direct O(n^2) force
//     summation on sampled particles, src/verification/oracle/particles/) and
//     the derived tolerance, from this file and nothing else -- never from the
//     kernels.
//
// VERSIONS. kVersion versions this SPECIFICATION: the unit's format and the
// check below. A change to either increments it. The workload's own bits are
// versioned separately, by Descriptor::version (workload.hpp), which moves
// whenever a configuration's output changes -- including when this
// specification does not.
//
// WHY A SHORT-RANGE FORCE WITH A CUTOFF. A neighbour search finds EXACTLY the
// pairs within the cutoff, so the oracle's direct sum over all pairs and the
// workload's neighbour sum are the same sum of the same terms; they differ
// only in the order the terms are added. (Barnes-Hut approximates, and its
// error against the direct sum is a property of the opening angle, not of
// arithmetic, so it could not be bounded by anything derived here.) The force
// has no singularity and no square root or transcendental, so every operation
// is correctly rounded and reproducible.
//
// A UNIT: THE FORCES ON n = kParticles PARTICLES IN A BOX
// -------------------------------------------------------
//   input   the positions: 3 n little-endian IEEE 754 doubles, x_0, y_0, z_0,
//           x_1, y_1, z_1, ... Element e of the unit (e = 3 p + c for
//           coordinate c in {0, 1, 2} of particle p) is
//             position_value(u64_of(block_at(seed, index, e)))
//           (primitives/rng/philox.hpp): (bits >> 11) * 2^-49, a multiple of
//           2^-49 in [0, 16 - 2^-49]. The lower end IS reached (0.0); the
//           upper end, kBoxEdge = 16, is not. Every correctly generated
//           coordinate is finite and below 16 -- the tolerance relies on it.
//           Only the first two words of each Philox block are used.
//   output  the force on each particle, 3 n doubles in the layout of the
//           input: F_p = (Fx, Fy, Fz). The box is open: no periodic wrap.
//
// THE FORCE. For particles i and j, d = x_i - x_j (a vector), q = |d|^2, and
// the cutoff radius is rc = 2 (kCutoffSquared = 4):
//
//   g(d) = max(0, 1 - q/4)^2 * d,        F_i = sum_{j != i} g(x_i - x_j).
//
// A smooth, bounded repulsion, zero at and beyond the cutoff, with g(0) = 0
// (two particles at one point exert nothing). The sum has the same terms for
// all j, in exact arithmetic: g(x_j - x_i) = -g(x_i - x_j), so the forces sum to
// zero (Newton's third law); that is a property the workload's own tests use.
// At the generator's density (one particle per unit volume) a particle has
// about 33 neighbours within the cutoff.
//
// THE EVALUATION MODEL. Both the workload and the oracle evaluate one term
// g(x_i - x_j) like this, and the tolerance below covers exactly this:
//   1. d_c = fl(x_ic - x_jc) for c = x, y, z;
//   2. q^ = fl(d_x^2 + d_y^2 + d_z^2): the three squares added in any order,
//      with or without fused multiply-add (so the result is within gamma_3 of
//      the sum of the squares rounded individually);
//   3. t^ = fl(1 - q^/4) (q^/4 is exact; a single subtraction, or the product
//      fused into it);
//   4. if t^ <= 0 the term is zero; otherwise w^ = fl(t^ t^), and component c
//      of the term is fl(w^ d_c). The last product may be fused into the
//      accumulation of the sum (a fused multiply-add whose product is the
//      exact w^ d_c). Nothing else in the term may be fused or reassociated:
//      in particular t is formed from the finished q^, not accumulated square
//      by square, because the intermediate values would be of size q and their
//      rounding would not be covered.
// The terms for one particle are added in any order, any association, any
// number of accumulators, with or without fused multiply-add into them: the
// kernel's choice for the ISA path and the decomposition. A term that is
// exactly zero adds nothing and rounds nothing. (-ffp-contract=off: fusing
// is by intrinsic only.)
//
// DECOMPOSITION: the number of cells per cutoff, one of kCellsPerCutoff,
// default kDefaultCellsPerCutoff: the neighbour search divides the box into
// cubic cells of edge rc / c (c = 1 gives 8 x 8 x 8 cells of edge 2; c = 4
// gives 32 x 32 x 32 of edge 1/2) and visits, for each particle, the cells
// within the cutoff. The order in which a particle's neighbours are visited
// -- and so in which its terms are added -- depends on c and on the ISA path.
//
// DETERMINISM: kBitExactFixedDecomposition. The same bits for one (cells per
// cutoff, ISA path) at any thread count and on any run; across c or across ISA
// paths, within the derived tolerance only (determinism.md §3). The scalar
// path additionally gives the same bits on x86-64 and ARM64. The oracle's
// contract is Contract::kBounded.
//
// THE CHECK, AND ITS BOUND
// ------------------------
// The oracle sums the force on kSampledParticles particles directly: for
// sampled particle p it evaluates all n - 1 terms g(x_p - x_j), j != p, with
// the model above, adding them in increasing j, plain scalar code (std::fma
// permitted). The sampled particles are kSampledParticles distinct particle
// numbers, a function of the unit index alone, so a re-run checks the same
// ones; the sample always includes particle 0 and particle n - 1. They are
// checked in increasing particle number, each particle's x, y, z in that
// order. An entry passes when residual = |F^_pc - F~_pc| <= T'_pc, the
// subtraction rounded in double.
//
// The verdict, in the terms of oracle.hpp:
//   * Agreement{kSampledParticles * 3} when every entry passes;
//   * BoundedMismatch for the FIRST entry, in the order above, that fails:
//     check "force-component", element = 3 p + c, residual, tolerance T'_pc,
//     blocks_compared the number of entries compared up to and including it.
//     Of several failing entries, the first in that order is named. A NaN or
//     infinity in the output entry makes the residual NaN or infinite, which
//     fails (`residual <= T'` is false for NaN);
//   * NotChecked (blocks_compared 0) when the evidence is not of the sizes
//     kInputBytes and kOutputBytes; or when any input coordinate is not finite
//     or is outside [0, kBoxEdge) (the tolerance cannot be derived; the
//     input-integrity bracket is what reports the corrupt input itself).
//     A NaN or infinity in the OUTPUT is not this: it is a BoundedMismatch.
// Every tolerance is at least kMinTolerance, 2^-1022: a particle with no
// neighbour in range (M = 0) has force exactly 0, and a nonzero output force
// then fails.
//
// Notation. u = kUnitRoundoff = 2^-53; gamma_k = k u / (1 - k u). For
// particle p, C_p is the set of CANDIDATES, the j != p whose oracle-computed
// q~_pj < 4 (1 + 2^-40), and M_p = |C_p| (an integer, at most n - 1).
//
// 1. The error of one term. Fix a pair and suppose the term is active, that
//   is the exact t = 1 - q/4 > 0 or the computed t^ > 0 (otherwise every
//   version of the term is exactly 0, and its error is 0).
//     (a) d^_c = d_c (1 + delta_c), |delta_c| <= u (the exact difference d_c
//         of two doubles is rounded once).
//     (b) The three squares carry relative error at most 2u + u^2 each, and
//         their sum of nonnegative terms, in any order with or without FMA,
//         at most gamma_3 more, so q^ = q (1 + psi) with |psi| <= gamma_5.
//     (c) With 1 - t = q/4: 1 - q^/4 = t (1 + psi) - psi, so
//           t^ - t = -psi (1 - t) + delta_t (t (1 + psi) - psi),  |delta_t| <= u.
//         In the active region q < 4 / (1 - gamma_5), so 1 - t = q/4 <= 1 + 2
//         gamma_5 (gamma_5 <= 1/2), and t <= 1, |t| <= 1 + 2 gamma_5 even
//         where t is slightly negative, so |t^ - t| <= (gamma_5 + u)(1 + 2
//         gamma_5) <= gamma_7 =: eps_t (gamma_5 + u is 6 u and gamma_7 is 7 u:
//         the headroom covers the second-order term).
//     (d) w = max(t, 0)^2 and w^ = fl(max(t^, 0)^2). max(., 0) is 1-Lipschitz
//         and both t and t^ are at most 1 + eps_t, so
//           |w^ - w| <= eps_t (2 + 2 eps_t) + u (1 + eps_t)^2
//                    <= (2 gamma_7 + u)(1 + gamma_7)^2 =: eps_w,
//         and w^ <= (1 + gamma_7)^2 (1 + u) =: W.
//     (e) In the active region q < 4 / (1 - gamma_5), so |d_c| <= 2 (1 +
//         gamma_5) and |d^_c| <= 2 (1 + gamma_6) =: D.
//     (f) The term component is g_c = w d_c and its computed value g^_c =
//         fl(w^ d^_c) (or the product exact, when fused into the sum):
//           g^_c - g_c = w^ d^_c delta_g + (w^ - w) d^_c + w (d^_c - d_c),
//         |delta_g| <= u, hence
//           |g^_c - g_c| <= E := D (u (1 + W) + eps_w)   (term_error()),
//         about 34 u = 3.8e-15, for the workload's term and the oracle's alike.
//   Candidates. A pair active in the exact term, in the workload's, or in the
//   oracle's has q~ < 4 (1 + 3 gamma_5) < 4 (1 + 2^-40), because every version
//   of q is within a relative gamma_5 of the exact one; so every active pair
//   is in C_p, and a pair outside C_p contributes exactly 0 to every version.
//
// 2. The error of a sum. For particle p and component c let S = sum over C_p
//   of |g_c| (exact terms). The workload and the oracle each add at most M_p
//   nonzero computed terms in some order, with or without FMA, so (Higham,
//   Accuracy and Stability of Numerical Algorithms, 2nd ed., sections 3.1
//   and 4.2) each is within M_p E + gamma_{M_p} (S + M_p E) of the exact sum,
//   the first term for the terms' own errors and the second for adding them
//   (sum |g^| <= S + M_p E). Hence
//     |F^ - F~| <= 2 (M_p E + gamma_{M_p} (S + M_p E)).
//   The oracle knows S~ = fl(sum |g~|) (plain, in the order of its sum), not
//   S; a sum of nonnegative terms has relative error at most gamma_{M_p}, and
//   each |g~| is within E of |g|, so S <= S~ / (1 - gamma_{M_p}) + M_p E. Thus
//     T_pc = 2 (M_p E + gamma_{M_p} (S~/(1 - gamma_{M_p}) + 2 M_p E)),
//   which force_tolerance(M_p, S~) evaluates. (M_p = 0: T = 0 before the
//   floor.) At M_p = 33 and a typical S~ of about 15 it is about 4e-13.
//
// 3. Evaluation. T as evaluated in double differs from the real number by a
//   relative 2^-37 at most (a product and a few sums of positive doubles). The
//   tolerance of a verdict is
//     T' = max((evaluated T) (1 + kEvaluationSlack), kMinTolerance),
//   kEvaluationSlack = 2^-36. So an output whose exact error is within T is
//   never rejected, and one whose exact error exceeds T (1 + 2^-34) is always
//   rejected (except where the floor intervenes). The oracle stream tests the
//   bound breached at (1 + epsilon) times T' and held at (1 - epsilon) times
//   it, epsilon = 2^-20, at M_p = 1 and at M_p of about 33.
//
// What the bound does not claim: an error below T (relative 1e-13 of a force
// of size 1) goes unseen, and only kSampledParticles of the n particles are
// examined; the force-sum invariant (the n forces sum to zero) is a property
// of the correct answer that the workload's own tests check, not part of this
// oracle.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::particles {

/// The name configuration and reports use: Descriptor::name.
inline constexpr std::string_view kName = "particles";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kParticles = 4096;
inline constexpr double kBoxEdge = 16.0;
inline constexpr double kCutoffSquared = 4.0;
inline constexpr std::size_t kInputBytes = 3 * kParticles * sizeof(double);
inline constexpr std::size_t kOutputBytes = 3 * kParticles * sizeof(double);

inline constexpr std::array<std::uint32_t, 3> kCellsPerCutoff = {1, 2, 4};
inline constexpr std::uint32_t kDefaultCellsPerCutoff = 2;

inline constexpr std::size_t kSampledParticles = 16;
inline constexpr std::size_t kEntriesChecked = 3 * kSampledParticles;

/// The coordinate a 64-bit draw makes: (bits >> 11) * 2^-49, a multiple of
/// 2^-49 in [0, 16). Exact: 2^-53 * 2^4 times an integer below 2^53.
[[nodiscard]] constexpr double position_value(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus49 = 1.0 / 562949953421312.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus49;
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_cells_per_cutoff(std::uint32_t cells) noexcept {
  return std::ranges::any_of(kCellsPerCutoff, [cells](std::uint32_t c) { return c == cells; });
}

/// u = 2^-53 and gamma_k = k u / (1 - k u), the constants of the bound.
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

/// The candidate margin: q~ < kCutoffSquared * (1 + 2^-40) puts a pair in C_p.
inline constexpr double kCandidateLimit = kCutoffSquared * (1.0 + 1.0 / 1099511627776.0);

/// E, the bound on the error of one component of one term (derivation 1(f)).
[[nodiscard]] constexpr double term_error() noexcept {
  const double d_max = 2.0 * (1.0 + gamma(6));
  const double w_max = (1.0 + gamma(7)) * (1.0 + gamma(7)) * (1.0 + kUnitRoundoff);
  const double eps_w = (2.0 * gamma(7) + kUnitRoundoff) * (1.0 + gamma(7)) * (1.0 + gamma(7));
  return d_max * (kUnitRoundoff * (1.0 + w_max) + eps_w);
}

/// T_pc of derivation 2 for a particle with `candidates` = M_p and the
/// oracle's `abs_sum` = S~, before the slack and the floor. Precondition:
/// candidates >= 1.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] constexpr double force_tolerance(std::size_t candidates, double abs_sum) noexcept {
  const auto m = static_cast<double>(candidates);
  const double e = term_error();
  const double g = gamma(candidates);
  return 2.0 * (m * e + g * (abs_sum / (1.0 - g) + 2.0 * m * e));
}

}  // namespace loadforge::workloads::particles

#endif
