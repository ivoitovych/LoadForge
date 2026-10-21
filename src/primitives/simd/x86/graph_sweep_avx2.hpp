// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_SIMD_X86_GRAPH_SWEEP_AVX2_HPP
#define LOADFORGE_PRIMITIVES_SIMD_X86_GRAPH_SWEEP_AVX2_HPP

// The graph workload's PageRank sweep for x86-64 AVX2
// (workloads/graph/kernels.hpp says what every sweep kernel computes).
//
// Built only on x86-64, and only when the build is not LOADFORGE_SIMD=scalar;
// the library that holds it defines LOADFORGE_SIMD_X86_KERNELS for whatever
// links it, so the workload's table names this function exactly when it
// exists. Like the dense-linear panel kernel it carries its own target
// attribute (avx2 only, no FMA: the sweep never fuses) instead of the unit
// being compiled with -mavx2 (gemm_avx2.hpp says why).
//
// Calling it on a CPU without AVX2 is the caller's error and ends the process
// with SIGILL: the path is chosen once, by select_path(), before the workload
// exists (simd/dispatch.hpp).

#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/kernels.hpp"

namespace loadforge::simd::x86 {

/// next[v] for v in [begin, end), with 4 or 8 accumulators (the vector widths;
/// the workload sweeps one and two accumulators with its scalar kernel). The
/// terms are gathered four to a vector, in increasing edge order.
void graph_sweep_avx2(const workloads::graph::InEdges& edges, std::span<const double> cur,
                      std::span<double> next, std::uint32_t accumulators, std::size_t begin,
                      std::size_t end) noexcept;

}  // namespace loadforge::simd::x86

#endif
