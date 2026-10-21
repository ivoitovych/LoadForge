// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_SIMD_X86_GEMM_AVX2_HPP
#define LOADFORGE_PRIMITIVES_SIMD_X86_GEMM_AVX2_HPP

// The dense-linear panel kernel for x86-64 AVX2 with FMA3
// (workloads/dense_linear/panel.hpp says what every panel kernel computes).
//
// Built only on x86-64, and only when the build is not LOADFORGE_SIMD=scalar;
// the library that holds it defines LOADFORGE_SIMD_X86_KERNELS for whatever
// links it, so a kernel table names this function exactly when it exists.
//
// The function carries its own target attribute instead of the translation
// unit being compiled with -mavx2 -mfma: an inline function from a header,
// instantiated in a unit built for AVX2, could be the copy the linker keeps
// for every caller, and would then execute AVX2 on a machine without it.
// With the attribute, only this function's own code is AVX2.
//
// Calling it on a CPU or kernel without AVX2 and FMA3 is the caller's error,
// and ends the process with SIGILL: the path is chosen once, by select_path(),
// before the workload exists (simd/dispatch.hpp).

#include "workloads/dense_linear/panel.hpp"

namespace loadforge::simd::x86 {

/// One panel update of one row tile: p_ij <- fma(a_ik, b_kj, p_ij) in
/// increasing k, from +0.0, then c_ij <- c_ij + p_ij.
void gemm_panel_avx2(const workloads::dense_linear::Panel& panel) noexcept;

}  // namespace loadforge::simd::x86

#endif
