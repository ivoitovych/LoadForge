// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_SIMD_ARM_GRAPH_SWEEP_NEON_HPP
#define LOADFORGE_PRIMITIVES_SIMD_ARM_GRAPH_SWEEP_NEON_HPP

// The graph workload's PageRank sweep for AArch64 Advanced SIMD
// (workloads/graph/kernels.hpp says what every sweep kernel computes).
//
// Built only on AArch64, and only when the build is not LOADFORGE_SIMD=scalar;
// the library that holds it defines LOADFORGE_SIMD_ARM_KERNELS for whatever
// links it, so the workload's table names this function exactly when it
// exists. Advanced SIMD is part of the AArch64 base architecture, so the unit
// needs no target flag.

#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/kernels.hpp"

namespace loadforge::simd::arm {

/// next[v] for v in [begin, end), with 2, 4 or 8 accumulators (the vector
/// widths; the workload sweeps one accumulator with its scalar kernel). NEON
/// has no gather, so each pair of ranks is loaded one lane at a time.
void graph_sweep_neon(const workloads::graph::InEdges& edges, std::span<const double> cur,
                      std::span<double> next, std::uint32_t accumulators, std::size_t begin,
                      std::size_t end) noexcept;

}  // namespace loadforge::simd::arm

#endif
