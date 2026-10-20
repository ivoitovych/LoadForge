// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AVX2 kernel at T1: panel.hpp's statement of what a kernel computes, entry by entry and bit
// for bit (panel_kernel_suite.hpp).

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
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  }
  static void kernel(const Panel& panel) { simd::x86::gemm_panel_avx2(panel); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Avx2, PanelKernelSuite, Avx2Kernel);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PanelKernelDeterminism);

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
