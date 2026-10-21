// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_PARTICLES_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_PARTICLES_HPP

// The particles workload (spec.hpp; workloads/workload.hpp).
//
// A unit is the forces on kParticles particles in a box, by a cell-list
// neighbour search and a vectorised short-range force sum. Declared class:
// kBitExactFixedDecomposition -- the same bits for one (cells per cutoff, ISA
// path) at any thread count and on any run. The decomposition is the cells per
// cutoff; the ISA path is part of it (determinism.md §3).
//
// Created by create(); the registry stream links it by name (kName).

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::particles {

/// A particles workload for `config`, or why not:
///   kConfiguration      a decomposition that is not 0 (the default) or one of
///                       kCellsPerCutoff, or an ISA path this build has no
///                       kernel for;
///   kFootprintTooSmall  not even one unit (input plus output) fits.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::particles

#endif
