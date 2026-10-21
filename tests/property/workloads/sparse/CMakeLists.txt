# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the property tests of src/workloads/sparse and nothing else.
loadforge_test_suite(loadforge_property_workloads_sparse_tests
  SOURCES
    property_test.cpp
  LINK loadforge_workloads_sparse)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_workloads_sparse_tests PRIVATE -fprofile-update=atomic)
endif()
