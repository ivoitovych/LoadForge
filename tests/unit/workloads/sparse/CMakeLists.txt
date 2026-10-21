# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the unit tests of src/workloads/sparse and nothing else.
loadforge_test_suite(loadforge_unit_workloads_sparse_tests
  SOURCES
    kernels_test.cpp
    create_test.cpp
    generate_test.cpp
    executor_mock_test.cpp
    workload_contract_test.cpp
  LINK loadforge_workloads_sparse)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_unit_workloads_sparse_tests PRIVATE -fprofile-update=atomic)
endif()
