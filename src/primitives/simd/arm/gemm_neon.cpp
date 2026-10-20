// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NEON panel kernel (gemm_neon.hpp). A kTileRows by kBlockColumns block of
// C is held as sixteen q-register accumulators -- four vectors of two doubles
// per row, half of the thirty-two registers -- so sixteen independent FMA
// chains are in flight. Per k: four B vectors loaded, four A entries
// broadcast, sixteen FMAs.
//
// Vectors move in and out of the byte buffers by std::memcpy, which the
// compiler turns into the same ldr q / str q a vld1q_f64 would be, without
// relying on how an implementation types the pointer its load builtin takes.
//
// The whole unit is guarded, so that a tool that reads every source file on a
// machine of the other architecture (clang-tidy over src/ on x86-64) sees an
// empty unit rather than a header it cannot compile.

#if defined(__aarch64__)

#include "primitives/simd/arm/gemm_neon.hpp"

#include <arm_neon.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::simd::arm {
namespace {

using workloads::dense_linear::kBlockColumns;
using workloads::dense_linear::kOrder;
using workloads::dense_linear::kTileRows;

constexpr std::size_t kLanes = 2;
constexpr std::size_t kVectors = kBlockColumns / kLanes;

using Block = std::array<float64x2_t, kVectors>;

[[nodiscard]] float64x2_t load(std::span<const std::byte> matrix, std::size_t index) noexcept {
  float64x2_t value = vdupq_n_f64(0.0);
  std::memcpy(&value, &matrix[index * sizeof(double)], sizeof value);
  return value;
}

void store(std::span<std::byte> matrix, std::size_t index, float64x2_t value) noexcept {
  std::memcpy(&matrix[index * sizeof(double)], &value, sizeof value);
}

/// One entry, in both lanes.
[[nodiscard]] float64x2_t broadcast(std::span<const std::byte> matrix, std::size_t index) noexcept {
  double value = 0.0;
  std::memcpy(&value, &matrix[index * sizeof(double)], sizeof value);
  return vdupq_n_f64(value);
}

}  // namespace

void gemm_panel_neon(const workloads::dense_linear::Panel& panel) noexcept {
  for (std::size_t column = 0; column < kOrder; column += kBlockColumns) {
    std::array<Block, kTileRows> sums{};  // value-initialised: every lane +0.0
    for (std::size_t k = panel.k0; k < panel.k0 + panel.depth; ++k) {
      Block b{};
      for (std::size_t vector = 0; vector < kVectors; ++vector) {
        b.at(vector) = load(panel.b, k * kOrder + column + vector * kLanes);
      }
      std::size_t row = panel.row;
      for (Block& sum : sums) {
        const float64x2_t a = broadcast(panel.a, row * kOrder + k);
        for (std::size_t vector = 0; vector < kVectors; ++vector) {
          // vfmaq_f64(p, a, b) is p + a * b with one rounding.
          sum.at(vector) = vfmaq_f64(sum.at(vector), a, b.at(vector));
        }
        ++row;
      }
    }
    std::size_t row = panel.row;
    for (const Block& sum : sums) {
      for (std::size_t vector = 0; vector < kVectors; ++vector) {
        const std::size_t at = row * kOrder + column + vector * kLanes;
        store(panel.c, at, vaddq_f64(load(panel.c, at), sum.at(vector)));
      }
      ++row;
    }
  }
}

}  // namespace loadforge::simd::arm

#endif
