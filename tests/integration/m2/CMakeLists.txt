# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T8 for the M2 integration -- the components of
# waves 1 and 2 linked as they are and run together, and against the real
# kernel (ADR 0003, rule 4). The supervision tier's counterpart, with the faulty
# worker and the isolation ladder, is tests/supervision.
loadforge_test_suite(loadforge_integration_m2_tests
  SOURCES
    pipeline_test.cpp
    sweep_test.cpp
    corrections_test.cpp
  LINK loadforge_controller loadforge_worker loadforge_workloads_registry
       loadforge_verification_oracle_registry loadforge_cli loadforge_controller_results
       loadforge_platform_fpenv loadforge_platform_isa loadforge_worker_executor)
target_compile_definitions(loadforge_integration_m2_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_m2_tests loadforge)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_integration_m2_tests PRIVATE -fprofile-update=atomic)
endif()
