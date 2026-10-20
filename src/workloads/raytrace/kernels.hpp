// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_RAYTRACE_KERNELS_HPP
#define LOADFORGE_WORKLOADS_RAYTRACE_KERNELS_HPP

// The row kernels behind the dispatch seam (primitives/simd/dispatch.hpp).
// Internal to the ray tracing workload.

#include <cstddef>
#include <cstdint>
#include <span>

#include "primitives/simd/dispatch.hpp"
#include "workloads/raytrace/scene.hpp"

namespace loadforge::workloads::raytrace {

/// Traces every pixel of row `y` against the hierarchy in `scene` and writes
/// the row's kWidth records to `row`.
using RowKernel = void (*)(const SceneStorage& scene, std::uint32_t y,
                           std::span<std::byte, kRowBytes> row) noexcept;

/// One kernel per path this build has: the scalar one, and the architecture's
/// wide one (AVX2 on x86-64, NEON on AArch64) unless the build is scalar-only.
[[nodiscard]] simd::KernelTable<RowKernel> row_kernels() noexcept;

}  // namespace loadforge::workloads::raytrace

#endif
