# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the fault-injection tests of src/workloads/sparse and nothing else.
loadforge_test_suite(loadforge_fault_injection_workloads_sparse_tests
  SOURCES
    fault_injection_test.cpp
  LINK loadforge_workloads_sparse)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_fault_injection_workloads_sparse_tests
                         PRIVATE -fprofile-update=atomic)
endif()
