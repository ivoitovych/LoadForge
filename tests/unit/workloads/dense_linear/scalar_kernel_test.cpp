// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scalar kernel against panel.hpp's statement of what a kernel computes
// (panel_kernel_suite.hpp): the product rounded and then the sum, no FMA.

#include "unit/workloads/dense_linear/panel_kernel_suite.hpp"
#include "workloads/dense_linear/panel.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

struct ScalarKernel {
  static constexpr bool kFused = false;
  static bool runs_here() { return true; }
  static void kernel(const Panel& panel) { panel_scalar(panel); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Scalar, PanelKernelSuite, ScalarKernel);
// The whole-product suite runs at T6, in tests/determinism/workloads/dense_linear/.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PanelKernelDeterminism);

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
