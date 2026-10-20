// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_STENCIL_KERNELS_HPP
#define LOADFORGE_WORKLOADS_STENCIL_KERNELS_HPP

// The sweep kernels of the stencil workload, one per ISA path, behind a
// KernelTable (primitives/simd/dispatch.hpp). Each updates a rectangle of
// INTERIOR cells of a Jacobi sweep (workloads/stencil/spec.hpp) from `src`
// into `dst`, both whole n x n grids of little-endian doubles, and touches no
// other cell: the ring and the tile walk belong to the caller.
//
// EVERY PATH EVALUATES THE SAME EXPRESSION, in the same association:
//
//   u'(i,j) = u(i,j) * 1/2 + ((N + S) + (E + W)) * 1/8
//
// with the vector kernels doing it a vector of cells at a time and the cells
// that do not fill a vector done by the same expression in scalar code. The
// weights are powers of two, so each product is exact and a fused multiply-add
// would round exactly as the separate multiply and add do; the association is
// the only thing that could make two paths differ, and it is the same.
// The kernels are written with the compilers' portable vector types and a
// function-level target, not with an ISA's intrinsics, so no intrinsic appears
// outside src/primitives/simd/x86 and arm (the portability workflow) and the
// one source builds on both architectures (docs/journal/23-workload-stencil.md).

#include <cstddef>
#include <span>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::workloads::stencil {

/// A rectangle of interior cells: rows [row_begin, row_end) and columns
/// [column_begin, column_end), all within [1, kEdge - 1).
struct Rect {
  std::size_t row_begin = 0;
  std::size_t row_end = 0;
  std::size_t column_begin = 0;
  std::size_t column_end = 0;
};

using SweepKernel = void (*)(std::span<const std::byte> src, std::span<std::byte> dst,
                             const Rect& rect) noexcept;

/// The kernels this build has, scalar always among them.
[[nodiscard]] simd::KernelTable<SweepKernel> kernel_table() noexcept;

}  // namespace loadforge::workloads::stencil

#endif
