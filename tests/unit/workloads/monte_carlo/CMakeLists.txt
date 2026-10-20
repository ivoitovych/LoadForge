# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/workloads/monte_carlo and nothing else.
loadforge_test_suite(loadforge_unit_workloads_monte_carlo_tests
  SOURCES
    executor_mock_test.cpp
    kernels_test.cpp
    workload_test.cpp
  LINK loadforge_workloads_monte_carlo)
