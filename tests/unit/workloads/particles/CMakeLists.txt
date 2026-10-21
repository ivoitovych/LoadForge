# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_test_suite(loadforge_unit_workloads_particles_tests
  SOURCES
    bin_test.cpp
    executor_mock_test.cpp
    grid_test.cpp
    kernel_test.cpp
    workload_test.cpp
  LINK loadforge_workloads_particles)
# Threads run under the coverage build; keep gcov's counters from going negative.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_unit_workloads_particles_tests PRIVATE -fprofile-update=atomic)
  target_compile_options(loadforge_workloads_particles PRIVATE -fprofile-update=atomic)
endif()
