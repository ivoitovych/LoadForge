// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_SIMD_ARM_GEMM_NEON_HPP
#define LOADFORGE_PRIMITIVES_SIMD_ARM_GEMM_NEON_HPP

// The dense-linear panel kernel for AArch64 Advanced SIMD
// (workloads/dense_linear/panel.hpp says what every panel kernel computes).
//
// Built only on AArch64, and only when the build is not LOADFORGE_SIMD=scalar;
// the library that holds it defines LOADFORGE_SIMD_ARM_KERNELS for whatever
// links it, so a kernel table names this function exactly when it exists.
// Advanced SIMD is part of the AArch64 base architecture, so the unit needs
// no target flag, and the double-precision FMA (vfmaq_f64) is in it.

#include "workloads/dense_linear/panel.hpp"

namespace loadforge::simd::arm {

/// One panel update of one row tile: p_ij <- fma(a_ik, b_kj, p_ij) in
/// increasing k, from +0.0, then c_ij <- c_ij + p_ij.
void gemm_panel_neon(const workloads::dense_linear::Panel& panel) noexcept;

}  // namespace loadforge::simd::arm

#endif
