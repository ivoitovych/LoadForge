# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T7/T8 for the M2 integration -- the components of
# waves 1 and 2 linked as they are and run together, and against the real
# kernel (ADR 0003, rule 4).
#
# The faulty worker is the shipped worker with a workload that corrupts its
# output on purpose; the ladder test runs the real controller against it. It
# is a test program only: nothing installs it and the shipped binary does not
# link it.
add_executable(loadforge_m2_faulty_worker faulty_worker.cpp)
target_link_libraries(loadforge_m2_faulty_worker PRIVATE loadforge_worker)
loadforge_set_warnings(loadforge_m2_faulty_worker)
loadforge_set_fp_contract(loadforge_m2_faulty_worker)
loadforge_enable_sanitizers(loadforge_m2_faulty_worker)
if(LOADFORGE_COVERAGE)
  target_link_options(loadforge_m2_faulty_worker PRIVATE --coverage)
endif()

loadforge_test_suite(loadforge_integration_m2_tests
  SOURCES
    ladder_test.cpp
    pipeline_test.cpp
  LINK loadforge_controller loadforge_worker loadforge_workloads_registry
       loadforge_verification_oracle_registry loadforge_verification_isolation
       loadforge_controller_results loadforge_cli)
target_compile_definitions(loadforge_integration_m2_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>"
  LOADFORGE_FAULTY_WORKER="$<TARGET_FILE:loadforge_m2_faulty_worker>")
add_dependencies(loadforge_integration_m2_tests loadforge loadforge_m2_faulty_worker)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_integration_m2_tests PRIVATE -fprofile-update=atomic)
endif()
