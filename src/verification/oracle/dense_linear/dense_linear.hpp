// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_DENSE_LINEAR_DENSE_LINEAR_HPP
#define LOADFORGE_VERIFICATION_ORACLE_DENSE_LINEAR_DENSE_LINEAR_HPP

// The independent oracle for the dense-linear workload (verification.md §3,
// §5): naive scalar dot products on sampled entries of C = AB, each judged
// against the tolerance verification/residual derives. Written from
// workloads/dense_linear/spec.hpp and from nothing else -- never from the
// kernels, which is the point of having it (verification.md §2, §3).
//
// WHAT IT CHECKS
// --------------
// For unit `index`, the n x n output (n = kOrder) is divided into grid x grid
// tiles of (n / grid) x (n / grid) entries, and ONE entry of each tile is
// checked: grid^2 entries. Every tile is REPRESENTED by one sampled entry; a
// fault is caught in a unit only if it alters a sampled entry. A fault
// confined to a tile does not, by that, meet a check. At the default grid of
// 8 each tile is 32 x 32 = 1024 entries and one is checked, so a fault that
// alters k entries of one tile, at positions independent of the sample, is
// caught in a single unit with probability k / 1024: a damaged 4 x 8 kernel
// output block (k = 32) 3.125%, four lanes of one row (k = 4) 0.390625%. Which
// entry of each tile is drawn from Philox at (kSampleSeed, index, tile), so
// the entries are a function of the unit index alone (oracle.hpp: a re-run
// checks the same ones) and different units check different entries; a
// PERSISTENT fault is therefore caught across units, a transient one in a
// single unit may be missed. The default grid checks 64 dot products of 256
// terms, about 0.2% of the unit's 2 n^3 operations, before the duty cycle
// (record.hpp, DutyCycle) thins it further. Sampling stays the default because
// a full check costs as much as the product. grid = n = 256 checks every
// entry, and only then does Agreement mean the whole output is right.
// Agreement carries blocks_compared, the number of entries checked (64, or
// 65536 at grid 256), so a consumer can tell "64 sampled entries passed" from
// "all entries passed"; it is not a proof of the remaining entries.
//
// For a checked entry (i, j), with a_i the row of A and b_j the column of B:
//
//     c' = sum_k a_ik b_kj,  S^ = sum_k |a_ik||b_kj|     (residual::dot)
//     tolerance = residual::tolerance(dot_model(n), S^)  (derivation there)
//     residual  = |c_ij - c'|
//
// and the entry is right when residual <= tolerance. The derivation covers
// every order the kernel may sum in, with or without FMA, and either result
// underflowing; the spec's gamma_n bound is its first step.
//
// THE VERDICT
// -----------
//   * The evidence is not kInputBytes in and kOutputBytes out: NotChecked.
//   * An input entry is NaN, or outside [-1, 1) -- the range the spec's
//     generator draws from: NotChecked, naming the entry. (The spec also says
//     "magnitude below 1", which entry_value(0) = -1 contradicts; the
//     generator is followed, and the contradiction is recorded in the
//     stream's journal.) The derivation relies on the bound (the magnitude
//     sum is then at most n), so outside it the tolerance cannot be derived;
//     and an input outside the spec's range is a corrupt input, which is the
//     input-integrity bracket's finding to report (oracle.hpp, §4a rule 4),
//     not a wrong output. The whole input is scanned, not only the rows and
//     columns the sample uses, so the verdict on an input does not depend on
//     which entries the index happened to sample. blocks_compared is 0.
//   * Every checked entry is within its tolerance: Agreement, blocks_compared
//     = grid^2.
//   * Otherwise BoundedMismatch for the worst entry: a NaN residual (a NaN or
//     an infinity in C, or an infinite difference) before any number, then the
//     greatest residual / tolerance, the first in tile order on a tie (two
//     ratios that overflow to infinity are a tie, as the records' ratio()
//     would print them). element
//     = i n + j; condition_number = S^ / |c'|, the dot product's relative
//     condition number, when c' is not zero; blocks_compared = grid^2, because
//     every sampled entry is compared before the verdict is given.
//
// THREAD-SAFETY: check() reads the evidence and allocates nothing but the
// verdict's text; it may run concurrently for any units.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::dense_linear {

/// The workload's name, as configuration and reports spell it.
inline constexpr std::string_view kWorkload = "dense_linear";
/// The default number of tiles along each side of C.
inline constexpr std::size_t kDefaultGrid = 8;
/// The Philox seed the sample is drawn under: a constant, so the sample
/// depends on the unit index and nothing else. "SAMPLEDL" in ASCII.
inline constexpr std::uint64_t kSampleSeed = 0x53414D504C45444CULL;

/// Whether `grid` divides the output into whole tiles: a power of two from 1
/// to kOrder.
[[nodiscard]] bool is_grid(std::size_t grid) noexcept;

/// An entry of C.
struct Entry {
  std::size_t row = 0;
  std::size_t column = 0;
};

/// The entry checked in tile `tile` (row-major over the grid x grid tiles) of
/// unit `index`. Preconditions: is_grid(grid), tile < grid * grid.
// The grid and the tile are both counts of tiles, named in the order the
// sentence above reads them; a struct for two integers would be one more type
// for a pure function.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Entry sample_entry(std::uint64_t index, std::size_t grid, std::size_t tile) noexcept;

/// The oracle for units of `plan`, checking grid x grid entries per unit.
/// kConfiguration when the plan's unit is not a dense-linear unit (its sizes,
/// or a decomposition the spec does not have), when the configuration asks for
/// a decomposition other than the one the plan resolved, or when `grid` is not
/// a valid grid.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> make_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan,
    std::size_t grid = kDefaultGrid);

}  // namespace loadforge::verification::oracle::dense_linear

#endif
