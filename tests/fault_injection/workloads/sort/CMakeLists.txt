# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_test_suite(loadforge_fault_injection_workloads_sort_tests
  SOURCES fault_injection_test.cpp
  LINK loadforge_workloads_sort)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_fault_injection_workloads_sort_tests PRIVATE -fprofile-update=atomic)
endif()
