// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MONTE_CARLO_MONTE_CARLO_HPP
#define LOADFORGE_WORKLOADS_MONTE_CARLO_MONTE_CARLO_HPP

// The Monte Carlo workload (spec.hpp): vectorised batches of Philox draws and
// integrand evaluations, estimating pi / 4. Provider of Workload
// (workloads/workload.hpp) for the name "monte_carlo".
//
// Its determinism class is kBitExactFixedDecomposition: the chunk length and
// the ISA path together fix every bit; the thread count and the executor's
// order of calls never reach them. The executor spreads the batches of a unit.
// compute() allocates nothing and needs no scratch.
//
// create() refuses, in this order:
//   * a path without a kernel in this build -- kConfiguration. Whether the
//     machine can run the path is the worker's question, answered once by
//     select_path() before this call; a workload that checked again would
//     be answering it differently from the record.
//   * a decomposition that is neither 0 nor one of kChunkLengths --
//     kConfiguration.
//   * a footprint under kInputBytes + kOutputBytes -- kFootprintTooSmall.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::monte_carlo {

[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::monte_carlo

#endif
