# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/workloads/stencil.
loadforge_test_suite(loadforge_unit_workloads_stencil_tests
  SOURCES
    stencil_test.cpp
    kernels_test.cpp
    executor_mock_test.cpp
  LINK loadforge_workloads_stencil)
