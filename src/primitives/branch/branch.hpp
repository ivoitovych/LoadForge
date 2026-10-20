// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_BRANCH_BRANCH_HPP
#define LOADFORGE_PRIMITIVES_BRANCH_BRANCH_HPP

// The branch calibration load (PLAN §6, layer 1): one data-dependent branch per
// element over generated values, so the predictor sees the pattern the
// threshold and the ordering give it. Random values against a threshold in the
// middle of the range defeat the predictor; the same values sorted, or a
// threshold at either end, are trivially predicted.
//
// THE DEFINITION. Unit u's values are v_j = u64_of(block_at(seed, u, j)) for j
// in [0, elements). Each value either is below `threshold` -- taken -- or not.
// The unit's result is
//   taken  = how many were taken,
//   below  = the wrapping sum of the taken values,
//   above  = the xor of the values not taken.
// `sorted` only changes the order the branch sees; every one of the three is a
// commutative reduction, so the result is the same either way. That identity is
// this primitive's metamorphic invariant, and the reason sorting is a legal
// way to make the same load predictable.
//
// Whether the compiler leaves the branch a branch (rather than a conditional
// move) is the toolchain's decision; the calibration records what it measured,
// it does not promise a mispredict per element.
//
// DETERMINISM: BitExact -- counts, wrapping sums and xors. The run combines the
// units by the same three reductions.
// THREAD-SAFETY: a running unit owns its slot's scratch exclusively.

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/result.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::branch {

inline constexpr workloads::DeterminismClass kDeterminism =
    workloads::DeterminismClass::kBitExact;

struct Config {
  std::uint64_t seed = 0;
  std::uint64_t elements = 0;
  /// A value is taken when it is strictly below this. Zero takes none; the
  /// largest value of the type takes all but a value equal to it.
  std::uint64_t threshold = 0;
  bool sorted = false;
};

struct Tally {
  std::uint64_t taken = 0;
  std::uint64_t below = 0;
  std::uint64_t above = 0;

  [[nodiscard]] bool operator==(const Tally&) const = default;
};

enum class ConfigError : std::uint8_t {
  kScratchTooSmall,  ///< fewer than `elements` values of scratch for every slot
};

/// The tally of one unit, over `scratch` (at least config.elements values).
/// Precondition: the scratch is large enough; run() checks it.
[[nodiscard]] Tally unit_tally(const Config& config, std::uint64_t unit,
                               std::span<std::uint64_t> scratch) noexcept;

/// Combines two tallies: counts and sums add, xors xor. Commutative and
/// associative, which is why the run's total does not depend on the order.
[[nodiscard]] Tally combine(const Tally& left, const Tally& right) noexcept;

/// Runs partials.size() units, each in its slot's share of `scratch`
/// (executor.threads() shares of config.elements values), writing unit i's
/// tally to partials[i], and returns the combination in index order.
[[nodiscard]] core::Result<Tally, ConfigError> run(worker::Executor& executor,
                                                   const Config& config,
                                                   std::span<std::uint64_t> scratch,
                                                   std::span<Tally> partials) noexcept;

}  // namespace loadforge::primitives::branch

#endif
