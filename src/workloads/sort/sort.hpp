// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_SORT_SORT_HPP
#define LOADFORGE_WORKLOADS_SORT_SORT_HPP

// The sort workload: a stable least-significant-digit radix sort of 2^17
// 64-bit keys, as spec.hpp specifies it. The output carries each key's input
// position, and equal keys keep input order, so the output is a function of the
// input alone -- the same bits for every decomposition, thread count and run.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sort {

/// The sort workload for `config`, or why the configuration is refused:
/// kConfiguration for a decomposition that is neither 0 nor in kChunkCounts, or
/// an ISA path other than scalar (the sort has no vector kernels);
/// kFootprintTooSmall when not even one unit fits the footprint.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::sort

#endif
