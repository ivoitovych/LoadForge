// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_GRAPH_GRAPH_HPP
#define LOADFORGE_WORKLOADS_GRAPH_GRAPH_HPP

// The graph workload (BFS distances and PageRank), as spec.hpp specifies it.
// The Registry's integration stream links create() under kName.

#include <memory>

#include "core/result.hpp"
#include "workloads/graph/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::graph {

/// Creates the workload for `config`, or refuses it:
///   kConfiguration      a decomposition not in kAccumulators (0 asks for
///                       kDefaultAccumulators), or an ISA path without a
///                       kernel in this build;
///   kFootprintTooSmall  not one unit fits config.footprint_bytes.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

/// What every graph workload says about itself, without creating one: the
/// descriptor of any workload create() returns (Registry::descriptor).
[[nodiscard]] Descriptor workload_descriptor();

}  // namespace loadforge::workloads::graph

#endif
