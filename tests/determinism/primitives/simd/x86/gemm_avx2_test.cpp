// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AVX2 kernel at T6: the whole product at every panel depth the specification lists, which is
// the fused model's bit for bit -- the model being std::fma, the same function on every
// architecture, so the AVX2 and NEON kernels give each other's bits as well
// (panel_kernel_suite.hpp).

#include "primitives/simd/x86/gemm_avx2.hpp"

#include "unit/workloads/dense_linear/panel_kernel_suite.hpp"
#include "workloads/dense_linear/panel.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

struct Avx2Kernel {
  static constexpr bool kFused = true;
  /// AVX2 and FMA3 as the CPU reports them, with the OS saving YMM state
  /// (GCC and Clang check XGETBV for AVX). A machine without them fails the
  /// suite rather than skipping it.
  static bool runs_here() {
    return __builtin_cpu_supports("avx2") != 0 && __builtin_cpu_supports("fma") != 0;
  }
  static void kernel(const Panel& panel) { simd::x86::gemm_panel_avx2(panel); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Avx2, PanelKernelDeterminism, Avx2Kernel);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PanelKernelSuite);

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
