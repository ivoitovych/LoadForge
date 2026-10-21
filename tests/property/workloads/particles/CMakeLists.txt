# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_test_suite(loadforge_property_workloads_particles_tests
  SOURCES
    pair_test.cpp
    property_test.cpp
  LINK loadforge_workloads_particles)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_workloads_particles_tests PRIVATE -fprofile-update=atomic)
endif()
