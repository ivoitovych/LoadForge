# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/workloads/adaptive_mesh and nothing else.
#
# WorkloadContract against the workload, its own unit tests, and
# ExecutorContract against this module's own executors (ADR 0003). support.hpp
# is shared with this module's other tiers.
loadforge_test_suite(loadforge_unit_workloads_adaptive_mesh_tests
  SOURCES
    workload_test.cpp
    executor_mock_test.cpp
  LINK loadforge_workloads_adaptive_mesh)
