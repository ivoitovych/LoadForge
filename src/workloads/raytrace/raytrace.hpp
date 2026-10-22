// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_RAYTRACE_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_RAYTRACE_HPP

// The ray tracing workload (spec.hpp): one 64 x 64 frame of 128 spheres and
// 128 triangles per unit, traced through a bounding volume hierarchy by
// packets of four rays.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::raytrace {

/// The workload for `config`, or why not: kConfiguration for a decomposition
/// that is not 0 or a tile edge (spec.hpp kTileEdges) or an ISA path this
/// build has no kernel for; kFootprintTooSmall when not even one unit's input
/// and output fit.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

/// What every ray tracing workload says about itself, without creating one: the
/// descriptor of any workload create() returns (Registry::descriptor).
[[nodiscard]] Descriptor workload_descriptor();

}  // namespace loadforge::workloads::raytrace

#endif
