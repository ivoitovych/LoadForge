// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MONTE_CARLO_SPEC_HPP
#define LOADFORGE_WORKLOADS_MONTE_CARLO_SPEC_HPP

// CONTRACT (ADR 0004). The specification the Monte Carlo workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md §2, §3; IMPLEMENTATION M4). Frozen: changed only by a
// contract change, never inside a stream.
//
//   The Monte Carlo stream writes the stress implementation: vectorised
//     batches of Philox draws and integrand evaluations, scalar and
//     vector kernels behind the dispatch seam (src/workloads/monte_carlo/).
//   The wave-2 oracle stream writes the oracle: a scalar replay of sampled
//     batches (src/verification/oracle/monte_carlo/) -- never from the
//     kernels.
//
// WHAT IS ESTIMATED
// -----------------
// pi / 4 = integral over [0, 1) of sqrt(1 - x^2) dx, by batches of
// kSamplesPerBatch samples. The integrand uses a multiplication, a
// subtraction and a square root and nothing else: each is correctly rounded
// by IEEE 754, so it reproduces exactly everywhere, where a libm
// transcendental would not (determinism.md §3).
//
// A UNIT
// ------
//   input   kBatches batch keys, each a little-endian u64: kInputBytes bytes.
//           Key b (b = 0 .. kBatches - 1) is batch_key(seed, index, b) =
//           u64_of(block_at(seed, index, b)): any value in [0, 2^64 - 1],
//           every one of which can occur, and every one of which is valid
//           input -- no input byte pattern is outside the domain.
//   samples Sample k of batch b (k = 0 .. kSamplesPerBatch - 1) is
//           f_k = sample_value(sample_bits(K_b, k)), K_b the batch key READ
//           FROM THE INPUT: the bits are words 0-1 (k even) or words 2-3
//           (k odd) of block_at(K_b, 0, k / 2), the lower-numbered word low;
//           x = unit_interval(bits), a multiple of 2^-53 in [0, 1 - 2^-53];
//           f_k = sqrt(1 - x * x), three operations, each rounded once to
//           nearest-even in double, no fused multiply-add (-ffp-contract=off
//           keeps the compiler from fusing; no kernel fuses on purpose). Every
//           path, kernel and oracle therefore computes the same f_k bits, and
//           f_k is in [2^-26, 1]: 1 at x = 0, 2^-26 at x = 1 - 2^-53, where
//           x * x rounds to 1 - 2^-52.
//   output  kBatches little-endian doubles: kOutputBytes bytes. Double b is
//           T_b, a floating-point sum of batch b's f_k, k = 0 ..
//           kSamplesPerBatch - 1, formed as DECOMPOSITION says.
//   What the numbers mean, for the workload's own property tests (not the
//   oracle's check): each f_k has mean pi / 4, to within 2^-50 (the grid of
//   x and the roundings), and variance 2 / 3 - pi^2 / 16 (about 0.0498), so
//   T_b / kSamplesPerBatch estimates pi / 4 and T_b has standard deviation
//   about 28.6 around kSamplesPerBatch * pi / 4 (about 12868.0).
//
// DECOMPOSITION: the chunk length C, one of kChunkLengths, default
// kDefaultChunkLength. Batch b's samples are taken in chunks of C consecutive
// k, chunk q being k in [q C, (q + 1) C). Each chunk is summed in the order
// the kernel of the ISA path in use defines -- a fixed function of (C, ISA
// path), never of the thread count or the data -- and the chunk sums are then
// added in increasing q, starting from chunk 0's sum. The executor may spread
// batches, or chunks of a batch, over threads; neither changes the bits. A
// configured decomposition of 0 resolves to kDefaultChunkLength; a value in
// kChunkLengths resolves to itself; every other value (neither 0 nor in
// kChunkLengths) is refused as kConfiguration by the workload and by the
// oracle. The plan reports the resolved C, and the oracle also refuses, as
// kConfiguration, a plan whose decomposition is not in kChunkLengths or whose
// input_bytes or output_bytes is not kInputBytes or kOutputBytes.
//
// DETERMINISM: kBitExactFixedDecomposition -- the same bits for one (C, ISA
// path) at any thread count and on any run; across C or across paths, within
// the tolerance below only (determinism.md §3, §5). The scalar path, which
// has no lanes, additionally gives the same bits on x86-64 and ARM64.
// Contract: kBounded.
//
// THE CHECK, AND ITS BOUND, IN FULL
// ---------------------------------
// The oracle replays kSampledBatches batches of each unit: the m-th is
// sampled_batch(index, m) = (index + 16 m) mod kBatches, m = 0 ..
// kSampledBatches - 1 -- four distinct batches, a function of the unit index
// alone, and any 16 consecutive units between them sample every batch. For
// each it computes the f_k from the input's batch key by plain scalar code,
// and r_b, their sum, by any method whose error is within the bound for
// floating-point summation below (recursive summation in any fixed order, a
// pairwise tree, compensated or exact summation all are). It then requires
//
//   residual_b = |T_b - r_b|  <=  tolerance_b = tolerance(r_b) = 2^-38 r_b.
//
// Derivation. Let S = kSamplesPerBatch = 2^14, u = 2^-53 (round to nearest,
// the FpEnvironment contract), E = sum_k f_k exactly, and
// gamma_n = n u / (1 - n u). Every f_k is the same double on both sides and
// non-negative, so sum |f_k| = E. Any sum of S terms by S - 1 floating-point
// additions, in any order or tree, satisfies |fl(sum) - E| <= gamma_{S-1} E
// (Higham, Accuracy and Stability of Numerical Algorithms, 2nd ed., §4.2,
// (4.6)). The workload's T_b is such a sum (the chunked order is a tree of
// S - 1 additions); the oracle's r_b is one or better. So
//   |T_b - r_b| <= 2 gamma_{S-1} E,   and   r_b >= (1 - gamma_{S-1}) E,
//   hence |T_b - r_b| <= 2 gamma_{S-1} / (1 - gamma_{S-1}) r_b
//                     =  2a / (1 - 2a) r_b,   a = (S - 1) u.
// And 2a / (1 - 2a) <= 2 S u holds exactly when 2 S (S - 1) u <= 1, that is
// 2 S (S - 1) <= 2^53 -- 536838144 <= 2^53, checked at compile time below.
// So |T_b - r_b| <= 2 S u r_b = 2^-38 r_b. Two roundings remain, and neither
// can move the verdict:
//   * tolerance(r_b) multiplies by a power of two: exact, since
//     r_b >= (1 - gamma_{S-1}) S 2^-26 > 2^-13, far from the subnormals.
//   * the subtraction: whenever |T_b - r_b| <= 2^-38 r_b, T_b lies in
//     [r_b / 2, 2 r_b], so T_b - r_b is exact (Sterbenz's lemma); outside
//     that interval the residual is at least r_b / 2, 2^37 times the
//     tolerance, before and after rounding.
// The computed comparison therefore decides the exact one: correct
// hardware always agrees, and the bound is breached by a residual of
// (1 + epsilon) times the tolerance and held at (1 - epsilon) times it, for
// any epsilon a test can represent (M2b exit criterion 3's form). The same
// bound, with r_b replaced by either result, bounds the difference between
// any two correct results: two chunk lengths, or two ISA paths (the T6 test
// of determinism.md §5). It is a rounding bound, not a statistical one: the
// oracle replays the same samples, so a statistical bound (a few times 28.6)
// would let through almost any corruption and false-alarm at its own
// significance level.
//
// VERDICTS
// --------
//   * evidence whose input or output is not kInputBytes or kOutputBytes
//     bytes: NotChecked, blocks_compared 0. No other NotChecked exists: every
//     input is in the domain, and r_b is always finite and above 2^-13.
//   * a sampled batch whose residual is not <= its tolerance -- larger, or
//     NaN because T_b is NaN; an infinite T_b gives an infinite residual -- is
//     a BoundedMismatch for the SMALLEST such batch number b (not the first
//     m): check "batch <b> sum" (b in decimal), residual the computed
//     |T_b - r_b|, tolerance tolerance(r_b), element b, no condition number
//     (the sum of non-negative terms is perfectly conditioned, and the bound
//     above does not use one), blocks_compared kSampledBatches.
//   * otherwise an Agreement, blocks_compared kSampledBatches. Every sampled
//     batch is replayed whatever the verdict, so blocks_compared counts
//     batches replayed and compared: always kSampledBatches.
// Batches not sampled are not checked in that unit; across units, each
// batch number is.
//
// VERSION: kVersion is the workload's Descriptor::version. It is raised by a
// contract change that alters the bytes any configuration produces -- the
// generator, the integrand, the sizes, the layout or the chunked order -- and
// by nothing else; a golden vector or error record names the version it was
// made under, and is never compared across versions. A change only to the
// oracle's sampling or wording leaves it alone.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "primitives/rng/philox.hpp"

namespace loadforge::workloads::monte_carlo {

inline constexpr std::string_view kName = "monte_carlo";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kBatches = 64;
inline constexpr std::size_t kSamplesPerBatch = std::size_t{1} << 14U;
inline constexpr std::size_t kInputBytes = kBatches * sizeof(std::uint64_t);
inline constexpr std::size_t kOutputBytes = kBatches * sizeof(double);
inline constexpr std::size_t kSampledBatches = 4;

inline constexpr std::array<std::uint32_t, 5> kChunkLengths = {64, 256, 1024, 4096, 16384};
inline constexpr std::uint32_t kDefaultChunkLength = 1024;

/// Whether a resolved decomposition is one the specification has. 0 is not:
/// it is a request for the default, which resolves to kDefaultChunkLength.
[[nodiscard]] constexpr bool is_chunk_length(std::uint32_t length) noexcept {
  return std::ranges::any_of(kChunkLengths,
                             [length](std::uint32_t chunk) { return chunk == length; });
}

/// Batch b's key in unit `index` under `seed`: what generate() writes at
/// input bytes [8 b, 8 b + 8).
[[nodiscard]] constexpr std::uint64_t batch_key(std::uint64_t seed, std::uint64_t index,
                                                std::uint64_t batch) noexcept {
  return rng::u64_of(rng::block_at(seed, index, batch));
}

/// The 64 random bits of sample k under a batch key: words 0-1 of block k / 2
/// for even k, words 2-3 for odd k, the lower-numbered word low.
[[nodiscard]] constexpr std::uint64_t sample_bits(std::uint64_t key,
                                                  std::uint64_t sample) noexcept {
  const rng::Block block = rng::block_at(key, 0, sample / 2);
  return sample % 2 == 0 ? rng::u64_of(block)
                         : std::uint64_t{block[2]} | (std::uint64_t{block[3]} << 32U);
}

/// The integrand at the point 64 random bits make: sqrt(1 - x * x),
/// x = unit_interval(bits), each operation rounded once. In [2^-26, 1].
[[nodiscard]] inline double sample_value(std::uint64_t bits) noexcept {
  const double x = rng::unit_interval(bits);
  const double square = x * x;
  return std::sqrt(1.0 - square);
}

/// The m-th batch the oracle replays in unit `index`, m < kSampledBatches.
[[nodiscard]] constexpr std::uint64_t sampled_batch(std::uint64_t index, std::size_t m) noexcept {
  return (index + (m * (kBatches / kSampledBatches))) % kBatches;
}

/// u = 2^-53, and the tolerance's scale 2 S u = 2^-38 (derivation above).
inline constexpr double kUnitRoundoff = 1.0 / 9007199254740992.0;
inline constexpr double kToleranceScale =
    2.0 * static_cast<double>(kSamplesPerBatch) * kUnitRoundoff;
static_assert(2 * kSamplesPerBatch * (kSamplesPerBatch - 1) <= (std::size_t{1} << 53U),
              "2a / (1 - 2a) <= 2 S u needs 2 S (S - 1) <= 2^53");

/// The tolerance for a batch whose replayed sum is `reference`: exact, a
/// multiplication by a power of two.
[[nodiscard]] constexpr double tolerance(double reference) noexcept {
  return kToleranceScale * reference;
}

}  // namespace loadforge::workloads::monte_carlo

#endif
