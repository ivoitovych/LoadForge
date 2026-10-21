// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AVX2 PageRank sweep (graph_sweep_avx2.hpp): four or eight accumulators
// held as one or two ymm vectors, filled by gathers of the ranks the in-edge
// tails name.
//
// Guarded like its ARM counterpart, so that the unit is empty wherever it
// could not be compiled.

#if defined(__x86_64__)

#include "primitives/simd/x86/graph_sweep_avx2.hpp"

#include <immintrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/kernels.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::simd::x86 {
namespace {

using workloads::graph::add_tail;
using workloads::graph::InEdges;
using workloads::graph::kEdgeShare;
using workloads::graph::kMaxAccumulators;
using workloads::graph::rank_of;

// NOLINTBEGIN(portability-simd-intrinsics)

/// The four ranks the tails at `at` .. `at` + 3 name, times the edge share.
__attribute__((target("avx2"))) inline __m256d terms_at(const InEdges& edges,
                                                        std::span<const double> cur,
                                                        std::size_t at) noexcept {
  const std::span<const std::uint32_t> group = edges.tails.subspan(at, 4);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the intrinsic's type.
  const __m128i index = _mm_loadu_si128(reinterpret_cast<const __m128i*>(group.data()));
  return _mm256_mul_pd(_mm256_i32gather_pd(cur.data(), index, sizeof(double)),
                       _mm256_set1_pd(kEdgeShare));
}

/// kLanes (4 or 8) accumulators as one or two vectors, filled by gathers.
template <std::size_t kLanes>
__attribute__((target("avx2"))) void sweep_wide(const InEdges& edges, std::span<const double> cur,
                                                std::span<double> next, std::uint32_t accumulators,
                                                std::size_t begin, std::size_t end) noexcept {
  for (std::size_t vertex = begin; vertex < end; ++vertex) {
    struct {
      __m256d low;
      __m256d high;  // lanes 4 .. 7: unused when kLanes is 4
    } acc = {_mm256_setzero_pd(), _mm256_setzero_pd()};
    const std::size_t first = edges.offsets[vertex];
    const std::size_t last = edges.offsets[vertex + 1];
    std::size_t term = first;
    for (; term + kLanes <= last; term += kLanes) {
      acc.low = _mm256_add_pd(acc.low, terms_at(edges, cur, term));
      if constexpr (kLanes == 8) {
        acc.high = _mm256_add_pd(acc.high, terms_at(edges, cur, term + 4));
      }
    }
    std::array<double, kMaxAccumulators> storage{};
    const std::span<double> lanes(storage);
    _mm256_storeu_pd(lanes.first(4).data(), acc.low);
    _mm256_storeu_pd(lanes.last(4).data(), acc.high);
    add_tail(edges, cur, lanes, term, last);
    next[vertex] = rank_of(lanes, accumulators);
  }
}

// NOLINTEND(portability-simd-intrinsics)

}  // namespace

__attribute__((target("avx2"))) void graph_sweep_avx2(const workloads::graph::InEdges& edges,
                                                      std::span<const double> cur,
                                                      std::span<double> next,
                                                      std::uint32_t accumulators, std::size_t begin,
                                                      std::size_t end) noexcept {
  if (accumulators == 4) {
    sweep_wide<4>(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_wide<8>(edges, cur, next, accumulators, begin, end);
  }
  // Leave the upper halves of the ymm registers clean for the SSE code the
  // caller runs next (gemm_avx2.cpp says what a dirty state costs).
  _mm256_zeroupper();
}

}  // namespace loadforge::simd::x86

#endif
