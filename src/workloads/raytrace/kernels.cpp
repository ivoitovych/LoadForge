// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/raytrace/kernels.hpp"

#include "workloads/raytrace/packet.hpp"
#include "workloads/raytrace/wide.hpp"

namespace loadforge::workloads::raytrace {
namespace {

void scalar_row(const SceneStorage& scene, std::uint32_t y,
                std::span<std::byte, kRowBytes> row) noexcept {
  trace_row<ScalarPacket>(scene, y, row);
}

#if !defined(LOADFORGE_SIMD_SCALAR) && defined(__x86_64__)
// AVX2 and nothing else: FMA3 is a separate feature and is not named, so no
// product here is ever fused (the specification forbids it).
[[gnu::target("avx2")]] void avx2_row(const SceneStorage& scene, std::uint32_t y,
                                      std::span<std::byte, kRowBytes> row) noexcept {
  trace_row<wide::Packet>(scene, y, row);
}
#elif !defined(LOADFORGE_SIMD_SCALAR) && defined(__aarch64__)
// Advanced SIMD is part of the AArch64 baseline: the vector types of wide.hpp
// are NEON registers with no attribute at all.
void neon_row(const SceneStorage& scene, std::uint32_t y,
              std::span<std::byte, kRowBytes> row) noexcept {
  trace_row<wide::Packet>(scene, y, row);
}
#endif

}  // namespace

simd::KernelTable<RowKernel> row_kernels() noexcept {
  simd::KernelTable<RowKernel> table;
  table.scalar = &scalar_row;
#if !defined(LOADFORGE_SIMD_SCALAR) && defined(__x86_64__)
  table.avx2 = &avx2_row;
#elif !defined(LOADFORGE_SIMD_SCALAR) && defined(__aarch64__)
  table.neon = &neon_row;
#endif
  return table;
}

}  // namespace loadforge::workloads::raytrace
