// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_GRAPH_KERNELS_HPP
#define LOADFORGE_WORKLOADS_GRAPH_KERNELS_HPP

// The PageRank sweep, behind the dispatch seam (primitives/simd/dispatch.hpp).
//
// One sweep computes r_it[v] = c + d * S for a range of vertices from r_{it-1},
// exactly as workloads/graph/spec.hpp pins it: the in-edges of v in increasing
// edge order, term q into accumulator q mod a, the accumulators combined left
// to right, d * S rounded before c is added. Every kernel computes the same
// bits for the same a -- the ISA path changes the speed and the instructions,
// never the arithmetic -- and a fused multiply-add is never used: the AVX2
// kernel is compiled without FMA, and the project builds with
// -ffp-contract=off for the NEON one. The vector kernels are in
// primitives/simd/x86/graph_sweep_avx2.hpp and primitives/simd/arm/
// graph_sweep_neon.hpp (the portability workflow refuses intrinsics anywhere
// else); this unit holds the scalar sweep and the table.

#include <cstddef>
#include <cstdint>
#include <span>

#include "primitives/simd/dispatch.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::workloads::graph {

/// The in-edges of every vertex, grouped by head: the tails of the edges whose
/// head is v, in increasing edge number, are tails[offsets[v] .. offsets[v+1]).
struct InEdges {
  std::span<const std::uint32_t> offsets;  ///< kVertices + 1 entries
  std::span<const std::uint32_t> tails;    ///< kEdges entries
};

/// Computes next[v] for v in [begin, end) from cur, with `accumulators` (one
/// of kAccumulators) partial sums per vertex. Reads cur everywhere, writes
/// only next[begin, end).
using SweepFn = void (*)(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                         std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept;

/// The widest decomposition: the accumulators of a vertex fit this many lanes.
inline constexpr std::size_t kMaxAccumulators = 8;

/// S, then r = c + d * S: the accumulators combined left to right, and d * S
/// rounded to double before the addition (the build never contracts them).
/// Every kernel, the vector ones in primitives/simd included, ends a vertex
/// with this.
[[nodiscard]] inline double rank_of(std::span<const double> lanes,
                                    std::uint32_t accumulators) noexcept {
  double sum = lanes[0];
  for (std::uint32_t lane = 1; lane < accumulators; ++lane) {
    sum += lanes[lane];
  }
  return kTeleport + kDamping * sum;
}

/// Adds the terms a vector loop left over -- fewer than a full group, term q
/// going to lane q mod a, which for a tail starting at a multiple of a is the
/// tail's own index -- to the lanes the vector accumulators were stored in.
inline void add_tail(const InEdges& edges, std::span<const double> cur, std::span<double> lanes,
                     std::size_t first, std::size_t last) noexcept {
  for (std::size_t term = first; term < last; ++term) {
    lanes[term - first] += cur[edges.tails[term]] * kEdgeShare;
  }
}

/// The sweep kernels this build has: scalar always, AVX2 on x86-64 and NEON on
/// AArch64 unless the build is scalar-only.
[[nodiscard]] const simd::KernelTable<SweepFn>& sweep_kernels() noexcept;

}  // namespace loadforge::workloads::graph

#endif
