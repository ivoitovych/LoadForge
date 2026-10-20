// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_DENSE_LINEAR_PANEL_HPP
#define LOADFORGE_WORKLOADS_DENSE_LINEAR_PANEL_HPP

// One panel update of one row tile: the unit of work every dense-linear
// kernel performs, and the one signature the KernelTable holds
// (simd/dispatch.hpp). The scalar kernel is beside this header; the AVX2
// kernel is under src/primitives/simd/x86/ and the NEON kernel under
// src/primitives/simd/arm/, because those are the only directories in which
// an ISA's intrinsics may appear.
//
// WHAT A KERNEL COMPUTES
// ----------------------
// For each of the kTileRows rows i in [row, row + kTileRows) and each column
// j in [0, kOrder):
//
//     c_ij <- c_ij + p_ij,   p_ij = sum over k in [k0, k0 + depth) of a_ik b_kj
//
// where p_ij starts at +0.0 and takes the products in increasing k, one at a
// time, and is added to c_ij once, after the last. That is the whole of the
// order: the columns are independent, so how a kernel groups them into
// registers cannot change a bit, and neither can which rows it is given. The
// kernels differ in one thing only, which is the ISA path's to decide
// (spec.hpp):
//
//   scalar  p <- p + (a * b): the product rounded, then the sum. No FMA:
//           the scalar kernel names no intrinsic, and -ffp-contract=off keeps
//           the compiler from fusing one for it.
//   avx2    p <- fma(a, b, p) by the AVX2 fused multiply-add: one rounding.
//   neon    p <- fma(a, b, p) by vfmaq_f64: one rounding.
//
// So the two fused paths compute the same chain and give the same bits as
// each other, and the scalar path gives different ones; across panel depths
// the chains are cut in different places, and the bits differ too. Each is
// within the bound spec.hpp derives (docs/journal/06-workload-dense-linear.md).
//
// MEMORY
// ------
// The matrices are the runner's byte buffers: little-endian doubles, row-major,
// kOrder by kOrder, with no double object ever created in them. A kernel reads
// and writes them only through copies (std::memcpy) or through the unaligned
// vector loads and stores, which both ISAs define as alias-safe, so no access
// is made through a double that is not there. Unaligned, because the
// workload contract promises cache-line alignment only to the start of each
// buffer and the tests do not even promise that.

#include <bit>
#include <cstddef>
#include <span>

#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear {

/// Rows per tile: the unit the executor spreads, and the register block's
/// height in every kernel.
inline constexpr std::size_t kTileRows = 4;
/// Columns per register block: two AVX2 vectors, four NEON vectors.
inline constexpr std::size_t kBlockColumns = 8;
/// Row tiles per unit: the executor's task count.
inline constexpr std::size_t kTiles = kOrder / kTileRows;

static_assert(kOrder % kTileRows == 0 && kOrder % kBlockColumns == 0);
// The kernels copy the buffers' bytes straight into doubles, which reads them
// as little-endian exactly when the machine is: both supported architectures
// are (docs/platforms.md), and a third that is not fails here, not in a result.
static_assert(std::endian::native == std::endian::little);

/// The arguments of one panel update.
///
///   a, b    the unit's A and B, all of each: kOrder * kOrder doubles.
///   c       the unit's C, all of it.
///   row     the tile's first row: a multiple of kTileRows below kOrder.
///   k0      the panel's first k; k0 + depth <= kOrder.
///   depth   the panel depth, at least 1.
///
/// Preconditions, which the workload keeps and a kernel does not check.
struct Panel {
  std::span<const std::byte> a;
  std::span<const std::byte> b;
  std::span<std::byte> c;
  std::size_t row = 0;
  std::size_t k0 = 0;
  std::size_t depth = 0;
};

/// A kernel: one panel update of one row tile.
using PanelKernel = void (*)(const Panel& panel) noexcept;

/// The portable kernel: no intrinsics, no FMA. Present in every build.
void panel_scalar(const Panel& panel) noexcept;

}  // namespace loadforge::workloads::dense_linear

#endif
