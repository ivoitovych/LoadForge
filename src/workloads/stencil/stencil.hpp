// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_STENCIL_STENCIL_HPP
#define LOADFORGE_WORKLOADS_STENCIL_STENCIL_HPP

// The stencil workload (workloads/stencil/spec.hpp): kSteps Jacobi sweeps of a
// 5-point stencil on a kEdge x kEdge grid, tiled over the executor with the
// sweep kernel of the configured ISA path.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::stencil {

/// The workload for `config`, or why it cannot be had:
///   kConfiguration      a decomposition that is not one of kTileEdges (0 asks
///                       for kDefaultTileEdge), or an ISA path this build has
///                       no kernel for;
///   kFootprintTooSmall  a footprint that holds no unit (kInputBytes +
///                       kOutputBytes each).
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::stencil

#endif
