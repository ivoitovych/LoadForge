# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/workloads/dense_linear and nothing else.
#
# WorkloadContract on every path this build has, the workload's own unit
# tests, the scalar kernel's, and ExecutorContract against this module's own
# executors (ADR 0003). support.hpp is shared with this module's other tiers.
loadforge_test_suite(loadforge_unit_workloads_dense_linear_tests
  SOURCES
    workload_test.cpp
    scalar_kernel_test.cpp
    executor_mock_test.cpp
  LINK loadforge_workloads_dense_linear)
