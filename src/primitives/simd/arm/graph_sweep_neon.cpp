// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NEON PageRank sweep (graph_sweep_neon.hpp): two, four or eight
// accumulators held as one to four q registers.
//
// The whole unit is guarded, so that a tool that reads every source file on a
// machine of the other architecture (clang-tidy over src/ on x86-64) sees an
// empty unit rather than a header it cannot compile.

#if defined(__aarch64__)

#include "primitives/simd/arm/graph_sweep_neon.hpp"

#include <arm_neon.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/kernels.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::simd::arm {
namespace {

using workloads::graph::add_tail;
using workloads::graph::InEdges;
using workloads::graph::kEdgeShare;
using workloads::graph::kMaxAccumulators;
using workloads::graph::rank_of;

// NOLINTBEGIN(portability-simd-intrinsics)

/// kLanes (2, 4 or 8) accumulators as kLanes / 2 vectors; NEON has no gather,
/// so each pair of ranks is loaded one lane at a time.
template <std::size_t kLanes>
void sweep_wide(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept {
  constexpr std::size_t kVectors = kLanes / 2;
  const float64x2_t share = vdupq_n_f64(kEdgeShare);
  for (std::size_t vertex = begin; vertex < end; ++vertex) {
    std::array<float64x2_t, kVectors> acc{};
    for (float64x2_t& vector : acc) {
      vector = vdupq_n_f64(0.0);
    }
    const std::size_t first = edges.offsets[vertex];
    const std::size_t last = edges.offsets[vertex + 1];
    std::size_t term = first;
    for (; term + kLanes <= last; term += kLanes) {
      for (std::size_t vector = 0; vector < kVectors; ++vector) {
        const std::size_t at = term + 2 * vector;
        const float64x2_t gathered =
            vcombine_f64(vld1_f64(&cur[edges.tails[at]]), vld1_f64(&cur[edges.tails[at + 1]]));
        acc[vector] = vaddq_f64(acc[vector], vmulq_f64(gathered, share));
      }
    }
    std::array<double, kMaxAccumulators> storage{};
    for (std::size_t vector = 0; vector < kVectors; ++vector) {
      vst1q_f64(std::span<double>(storage).subspan(2 * vector, 2).data(), acc[vector]);
    }
    const std::span<double> lanes(storage);
    add_tail(edges, cur, lanes, term, last);
    next[vertex] = rank_of(lanes, accumulators);
  }
}

// NOLINTEND(portability-simd-intrinsics)

}  // namespace

void graph_sweep_neon(const workloads::graph::InEdges& edges, std::span<const double> cur,
                      std::span<double> next, std::uint32_t accumulators, std::size_t begin,
                      std::size_t end) noexcept {
  if (accumulators == 2) {
    sweep_wide<2>(edges, cur, next, accumulators, begin, end);
  } else if (accumulators == 4) {
    sweep_wide<4>(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_wide<8>(edges, cur, next, accumulators, begin, end);
  }
}

}  // namespace loadforge::simd::arm

#endif
