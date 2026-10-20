# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/workloads/graph and nothing else.
loadforge_test_suite(loadforge_unit_workloads_graph_tests
  SOURCES
    graph_test.cpp
    workload_contract_test.cpp
    executor_mock_test.cpp
  LINK loadforge_workloads_graph)
# The coverage build runs threads (the contract's executors, the mock's): the
# counters of a thread are not atomic unless they are asked to be.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_unit_workloads_graph_tests PRIVATE -fprofile-update=atomic)
  target_compile_options(loadforge_workloads_graph PRIVATE -fprofile-update=atomic)
endif()
