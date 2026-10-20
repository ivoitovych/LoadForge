# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/workloads/graph and nothing else.
loadforge_test_suite(loadforge_property_workloads_graph_tests
  SOURCES property_test.cpp
  LINK loadforge_workloads_graph)
target_include_directories(loadforge_property_workloads_graph_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/unit/workloads/graph")
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_workloads_graph_tests PRIVATE -fprofile-update=atomic)
  target_compile_options(loadforge_workloads_graph PRIVATE -fprofile-update=atomic)
endif()
