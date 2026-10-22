// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_SPARSE_SPARSE_HPP
#define LOADFORGE_WORKLOADS_SPARSE_SPARSE_HPP

// The sparse (conjugate gradient) workload, as spec.hpp specifies it and
// workload.hpp lets a workload be: a family of independent units, each a
// symmetric positive-definite banded-sparse system solved by exactly
// kIterations steps of unpreconditioned conjugate gradients, the iterate and
// its explicit residual as the output.
//
// DETERMINISM: BitExactFixedDecomposition. A unit's output depends on the
// chunk rows (WorkloadConfig::decomposition) and the ISA path, and on nothing
// else: not on the executor's thread count, on which thread ran which chunk,
// or on what the scratch held. The chunks are fixed by the decomposition, each
// chunk's partial sums are made by the path's kernel in its own order, and the
// partials are combined by a tree whose shape is the specification's.
//
// PATHS. Scalar only, today: Descriptor::paths is what the kernel table holds.
// The vector kernels (AVX2 with FMA3, NEON) are intrinsics, which the
// portability workflow admits only under src/primitives/simd/x86/ and arm/, and
// this stream owns neither (docs/journal/24-workload-sparse.md, "Contract
// questions"). Their place in the seam is the table in sparse.cpp: a kernel
// set added to it is a path the workload offers, and nothing else changes.

#include <memory>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sparse {

/// The workload for `config`, or why it cannot be made:
///   kConfiguration      an ISA path the kernel table has no kernels for, or a
///                       decomposition that is not 0 (the default,
///                       kDefaultChunkRows) or one of kChunkRows;
///   kFootprintTooSmall  not one unit's input and output fit the footprint.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

/// What every sparse workload says about itself, without creating one: the
/// descriptor of any workload create() returns (Registry::descriptor).
[[nodiscard]] Descriptor workload_descriptor();

}  // namespace loadforge::workloads::sparse

#endif
