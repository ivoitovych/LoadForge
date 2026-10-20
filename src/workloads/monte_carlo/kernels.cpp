// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/monte_carlo/kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "workloads/monte_carlo/spec.hpp"

// The vector kernels are not written with intrinsics: those may live only
// under primitives/simd/x86/ (portability workflow), which this module does
// not own. They are the same lane-structured loop, compiled for the path's
// instruction set -- the lane count fixes the bits whether or not the
// compiler turns the lanes into vector registers.
namespace loadforge::workloads::monte_carlo {

namespace {

/// The lane-structured sum. `Path` is a type local to the translation unit,
/// so each instantiation has internal linkage: a copy compiled for one
/// instruction set can never be merged with, and replace, another's.
template <typename Path>
[[gnu::always_inline]] inline double lane_sum(std::uint64_t key, std::uint64_t first,
                                              std::uint32_t length) noexcept {
  constexpr std::size_t kLanes = Path::kLanes;
  // Pairs of samples one block yields, per step: the lanes always take whole
  // blocks (a lane count of 1 takes one block into its single lane).
  constexpr std::size_t kPairs = kLanes > 1 ? kLanes / 2 : 1;
  std::array<double, kLanes> lane{};
  const std::uint64_t end = first + length;
  for (std::uint64_t k = first; k < end; k += 2 * kPairs) {
    for (std::size_t pair = 0; pair < kPairs; ++pair) {
      const rng::Block block = rng::block_at(key, 0, k / 2 + pair);
      const std::uint64_t odd = std::uint64_t{block[2]} | (std::uint64_t{block[3]} << 32U);
      lane.at((2 * pair) % kLanes) += sample_value(rng::u64_of(block));
      lane.at((2 * pair + 1) % kLanes) += sample_value(odd);
    }
  }
  // From +0.0, so every lane count enters the loop at least once: the first
  // addition is exact (the lanes hold positive sums), the order is lane 0, 1, ...
  double total = 0.0;
  for (std::size_t index = 0; index < kLanes; ++index) {
    total += lane.at(index);
  }
  return total;
}

struct ScalarPath {
  static constexpr std::size_t kLanes = 1;
};

double scalar_chunk(std::uint64_t key, std::uint64_t first, std::uint32_t length) noexcept {
  return lane_sum<ScalarPath>(key, first, length);
}

#if defined(__x86_64__) && !defined(LOADFORGE_SIMD_SCALAR)

struct Avx2Path {
  static constexpr std::size_t kLanes = 4;
};

[[gnu::target("avx2")]] double avx2_chunk(std::uint64_t key, std::uint64_t first,
                                          std::uint32_t length) noexcept {
  return lane_sum<Avx2Path>(key, first, length);
}

#elif defined(__aarch64__) && !defined(LOADFORGE_SIMD_SCALAR)

struct NeonPath {
  static constexpr std::size_t kLanes = 2;
};

double neon_chunk(std::uint64_t key, std::uint64_t first, std::uint32_t length) noexcept {
  return lane_sum<NeonPath>(key, first, length);
}

#endif

}  // namespace

simd::KernelTable<ChunkKernel> chunk_kernels() noexcept {
  simd::KernelTable<ChunkKernel> table;
  table.scalar = &scalar_chunk;
#if defined(__x86_64__) && !defined(LOADFORGE_SIMD_SCALAR)
  table.avx2 = &avx2_chunk;
#elif defined(__aarch64__) && !defined(LOADFORGE_SIMD_SCALAR)
  table.neon = &neon_chunk;
#endif
  return table;
}

}  // namespace loadforge::workloads::monte_carlo
