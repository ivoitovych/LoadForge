// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AVX2 panel kernel (gemm_avx2.hpp). A kTileRows by kBlockColumns block of
// C is held as eight ymm accumulators -- two vectors of four doubles per row
// -- so eight independent FMA chains are in flight, which is what two FMA
// ports with a four-cycle latency need to stay busy. Per k: two B vectors
// loaded, four A entries broadcast, eight FMAs.
//
// Guarded like its ARM counterpart, so that the unit is empty wherever it
// could not be compiled.

#if defined(__x86_64__)

#include "primitives/simd/x86/gemm_avx2.hpp"

#include <immintrin.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::simd::x86 {
namespace {

using workloads::dense_linear::kBlockColumns;
using workloads::dense_linear::kOrder;
using workloads::dense_linear::kTileRows;

constexpr std::size_t kLanes = 4;

/// One row of the register block: columns [0, 4) and [4, 8). A struct, not a
/// std::array, because a vector type's attributes do not survive being a
/// template argument.
struct Sums {
  __m256d low;
  __m256d high;
};

/// Four doubles from `index` on: _mm256_loadu_pd reads through a may_alias
/// type in both GCC and Clang, so the buffer needs no double objects in it.
[[nodiscard]] __attribute__((target("avx2,fma"))) __m256d load(std::span<const std::byte> matrix,
                                                               std::size_t index) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return _mm256_loadu_pd(reinterpret_cast<const double*>(&matrix[index * sizeof(double)]));
}

__attribute__((target("avx2,fma"))) void store(std::span<std::byte> matrix, std::size_t index,
                                               __m256d value) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  _mm256_storeu_pd(reinterpret_cast<double*>(&matrix[index * sizeof(double)]), value);
}

/// One entry, broadcast to all four lanes.
[[nodiscard]] __attribute__((target("avx2,fma"))) __m256d
broadcast(std::span<const std::byte> matrix, std::size_t index) noexcept {
  double value = 0.0;
  std::memcpy(&value, &matrix[index * sizeof(double)], sizeof value);
  return _mm256_set1_pd(value);
}

}  // namespace

__attribute__((target("avx2,fma"))) void gemm_panel_avx2(
    const workloads::dense_linear::Panel& panel) noexcept {
  static_assert(kBlockColumns == 2 * kLanes);
  for (std::size_t column = 0; column < kOrder; column += kBlockColumns) {
    std::array<Sums, kTileRows> sums{};  // value-initialised: every lane +0.0
    for (std::size_t k = panel.k0; k < panel.k0 + panel.depth; ++k) {
      const __m256d low = load(panel.b, k * kOrder + column);
      const __m256d high = load(panel.b, k * kOrder + column + kLanes);
      std::size_t row = panel.row;
      for (Sums& sum : sums) {
        const __m256d a = broadcast(panel.a, row * kOrder + k);
        sum.low = _mm256_fmadd_pd(a, low, sum.low);
        sum.high = _mm256_fmadd_pd(a, high, sum.high);
        ++row;
      }
    }
    std::size_t row = panel.row;
    for (const Sums& sum : sums) {
      const std::size_t at = row * kOrder + column;
      store(panel.c, at, _mm256_add_pd(load(panel.c, at), sum.low));
      store(panel.c, at + kLanes, _mm256_add_pd(load(panel.c, at + kLanes), sum.high));
      ++row;
    }
  }
  // Leave the upper halves of the ymm registers clean for the SSE code the
  // caller runs next. An optimising build inserts this by itself; an
  // unoptimised one does not, and every SSE instruction after the kernel then
  // paid for the dirty state: the tests' scalar model ran fifty times slower.
  _mm256_zeroupper();
}

}  // namespace loadforge::simd::x86

#endif
