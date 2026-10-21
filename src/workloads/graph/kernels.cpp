// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/graph/kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/graph/spec.hpp"

#if defined(LOADFORGE_SIMD_X86_KERNELS)
#include "primitives/simd/x86/graph_sweep_avx2.hpp"
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
#include "primitives/simd/arm/graph_sweep_neon.hpp"
#endif

namespace loadforge::workloads::graph {
namespace {

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

#if defined(LOADFORGE_SIMD_X86_KERNELS)
/// The vector kernel holds the widths of a vector. One and two accumulators
/// are narrower than an AVX2 vector: the scalar sweep is the AVX2 path's, and
/// the same bits.
void sweep_avx2(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept {
  if (accumulators >= 4) {
    simd::x86::graph_sweep_avx2(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_scalar(edges, cur, next, accumulators, begin, end);
  }
}
#endif

#if defined(LOADFORGE_SIMD_ARM_KERNELS)
/// One accumulator is narrower than a NEON vector: the scalar sweep.
void sweep_neon(const InEdges& edges, std::span<const double> cur, std::span<double> next,
                std::uint32_t accumulators, std::size_t begin, std::size_t end) noexcept {
  if (accumulators >= 2) {
    simd::arm::graph_sweep_neon(edges, cur, next, accumulators, begin, end);
  } else {
    sweep_scalar(edges, cur, next, accumulators, begin, end);
  }
}
#endif

}  // namespace

const simd::KernelTable<SweepFn>& sweep_kernels() noexcept {
  static constexpr simd::KernelTable<SweepFn> kTable = {
      .scalar = &sweep_scalar,
#if defined(LOADFORGE_SIMD_X86_KERNELS)
      .avx2 = &sweep_avx2,
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
      .neon = &sweep_neon,
#endif
  };
  return kTable;
}

}  // namespace loadforge::workloads::graph
