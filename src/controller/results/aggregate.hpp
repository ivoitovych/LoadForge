// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_RESULTS_AGGREGATE_HPP
#define LOADFORGE_CONTROLLER_RESULTS_AGGREGATE_HPP

// What a run's parts add up to: the helpers the controller uses to build a
// RunResult out of what its workers and its sampler gave it, and that
// derive_verdict() reads it back with.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "controller/results/result.hpp"
#include "statistics/signature.hpp"
#include "verification/runner.hpp"

namespace loadforge::results {

/// The findings of a run, by what they say.
///
///   wrong_results   records that report a wrong result (ExactBitCorruption,
///                   NumericalVerificationFailure).
///   not_performed   VerificationNotPerformed records: a check that was owed
///                   and not made.
struct FindingCounts {
  std::size_t wrong_results = 0;
  std::size_t not_performed = 0;
};

[[nodiscard]] FindingCounts count_findings(const RunResult& result);

/// The sum of every workload's tally, field by field.
[[nodiscard]] verification::Tally total_tally(const std::vector<WorkloadResult>& workloads);

/// Units computed times the plan's logical bytes per unit (F12), saturating at
/// the largest count a 64-bit field holds. A product that does not fit is
/// reported as the ceiling, never as a wrapped small number: a bandwidth
/// computed from it is then visibly absurd rather than plausibly wrong.
[[nodiscard]] std::uint64_t logical_bytes(std::uint64_t units_computed,
                                          std::uint64_t bytes_per_unit) noexcept;

/// Whether any series named `name` in the signature is a counter that moved:
/// a delta above zero. A gauge, an unavailable series and a counter that did
/// not move are all "no".
[[nodiscard]] bool counter_advanced(const statistics::LoadSignature& signature,
                                    std::string_view name);

/// Sets the schema version and the verdict a result's contents imply, the
/// last thing done to a result before it is written.
void seal(RunResult& result);

}  // namespace loadforge::results

#endif
