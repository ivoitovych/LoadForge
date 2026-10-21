// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_MONTE_CARLO_MONTE_CARLO_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_MONTE_CARLO_MONTE_CARLO_ORACLE_HPP

// The independent oracle for the Monte Carlo workload, written from
// src/workloads/monte_carlo/spec.hpp and from nothing else: a scalar replay of
// the four batches the specification samples from each unit, each sum judged
// against the tolerance the specification derives.

#include <cstdint>
#include <memory>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle {

/// The sum of the kSamplesPerBatch integrand values of the batch whose key is
/// `key`, by compensated (Neumaier) summation: its error is far inside the
/// bound the specification allows a replay (a few units of 2^-53 times the
/// sum, against 2^-38 times it). Exposed so a test can check the summation
/// against an external exact sum.
[[nodiscard]] double replay_batch_sum(std::uint64_t key);

/// The Monte Carlo oracle for units of `plan`, or kConfiguration when the
/// configuration's decomposition is neither 0 nor a chunk length, or the
/// plan's is not a chunk length or its sizes are not a Monte Carlo unit's.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> make_monte_carlo_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle

#endif
