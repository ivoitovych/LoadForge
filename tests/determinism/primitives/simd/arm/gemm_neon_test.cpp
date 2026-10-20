// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NEON kernel at T6: the whole product at every panel depth the specification lists, which is
// the fused model's bit for bit -- the model being std::fma, the same function on every
// architecture, so the AVX2 and NEON kernels give each other's bits as well
// (panel_kernel_suite.hpp).

#include "primitives/simd/arm/gemm_neon.hpp"

#include "unit/workloads/dense_linear/panel_kernel_suite.hpp"
#include "workloads/dense_linear/panel.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

struct NeonKernel {
  static constexpr bool kFused = true;
  /// Advanced SIMD is part of the AArch64 base architecture.
  static bool runs_here() { return true; }
  static void kernel(const Panel& panel) { simd::arm::gemm_panel_neon(panel); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Neon, PanelKernelDeterminism, NeonKernel);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PanelKernelSuite);

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
