# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 for src/workloads/raytrace and nothing else.
#
# The workload against WorkloadContract (which it provides), its own executors
# against ExecutorContract (which it consumes), the factory and the output
# layout, the generator, the hierarchy's structure and the two kernels'
# agreement on crafted scenes.
loadforge_test_suite(loadforge_unit_workloads_raytrace_tests
  SOURCES
    bvh_test.cpp
    contract_test.cpp
    generator_test.cpp
    kernels_test.cpp
    workload_test.cpp
  LINK loadforge_workloads_raytrace)

# Tasks run on several threads here: atomic counters, as for the library.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_unit_workloads_raytrace_tests PRIVATE -fprofile-update=atomic)
endif()
