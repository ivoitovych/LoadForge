// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/graph/kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/spec.hpp"

#if defined(__x86_64__) && !defined(LOADFORGE_SIMD_SCALAR)
#define LOADFORGE_GRAPH_AVX2
#include <immintrin.h>
#elif defined(__aarch64__) && !defined(LOADFORGE_SIMD_SCALAR)
#define LOADFORGE_GRAPH_NEON
#include <arm_neon.h>
#endif

namespace loadforge::workloads::graph {
namespace {

/// The widest decomposition: the accumulators of a vertex fit this many lanes.
constexpr std::size_t kMaxAccumulators = 8;

/// S, then r = c + d * S: the accumulators combined left to right, and d * S
/// rounded to double before the addition (the build never contracts them).
[[nodiscard]] double rank_of(std::span<const double> lanes, std::uint32_t accumulators) noexcept {
  double sum = lanes[0];
  for (std::uint32_t lane = 1; lane < accumulators; ++lane) {
    sum += lanes[lane];
  }
  return kTeleport + kDamping * sum;
}

/// Adds the terms the vector loop left over -- fewer than a full group, term
/// q going to lane q mod a, which for a tail starting at a multiple of a is
/// the tail's own index -- to the lanes the vector accumulators were stored in.
void add_tail(const InEdges& edges, std::span<const double> cur, std::span<double> lanes,
              std::size_t first, std::size_t last) noexcept {
  for (std::size_t term = first; term < last; ++term) {
    lanes[term - first] += cur[edges.tails[term]] * kEdgeShare;
  }
}

void sweep_scalar(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                  std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept {
  const std::size_t mask = accumulators - 1U;  // a is a power of two
  for (std::size_t vertex = begin; vertex < end; ++vertex) {
    std::array<double, kMaxAccumulators> storage{};
    const std::span<double> lanes(storage);
    const std::size_t first = edges.offsets[vertex];
    const std::size_t last = edges.offsets[vertex + 1];
    for (std::size_t term = first; term < last; ++term) {
      lanes[(term - first) & mask] += cur[edges.tails[term]] * kEdgeShare;
    }
    next[vertex] = rank_of(lanes, accumulators);
  }
}

// The kernels are the one place an intrinsic is wanted, behind the dispatch seam.
// NOLINTBEGIN(portability-simd-intrinsics)
#if defined(LOADFORGE_GRAPH_AVX2)

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

__attribute__((target("avx2"))) void sweep_avx2(const InEdges& edges, std::span<const double> cur,
                                                std::span<double> next, std::uint32_t accumulators,
                                                std::size_t begin, std::size_t end) noexcept {
  // One and two accumulators are narrower than a vector: the scalar sweep is
  // the AVX2 path's, and the same bits.
  if (accumulators == 4) {
    sweep_wide<4>(edges, cur, next, accumulators, begin, end);
  } else if (accumulators == 8) {
    sweep_wide<8>(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_scalar(edges, cur, next, accumulators, begin, end);
  }
}

#endif

#if defined(LOADFORGE_GRAPH_NEON)

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

void sweep_neon(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept {
  if (accumulators == 2) {
    sweep_wide<2>(edges, cur, next, accumulators, begin, end);
  } else if (accumulators == 4) {
    sweep_wide<4>(edges, cur, next, accumulators, begin, end);
  } else if (accumulators == 8) {
    sweep_wide<8>(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_scalar(edges, cur, next, accumulators, begin, end);
  }
}

#endif
// NOLINTEND(portability-simd-intrinsics)

}  // namespace

const simd::KernelTable<SweepFn>& sweep_kernels() noexcept {
  static constexpr simd::KernelTable<SweepFn> kTable = {
      .scalar = &sweep_scalar,
#if defined(LOADFORGE_GRAPH_AVX2)
      .avx2 = &sweep_avx2,
#endif
#if defined(LOADFORGE_GRAPH_NEON)
      .neon = &sweep_neon,
#endif
  };
  return kTable;
}

}  // namespace loadforge::workloads::graph
