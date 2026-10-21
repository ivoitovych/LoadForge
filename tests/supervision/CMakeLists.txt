# SPDX-License-Identifier: GPL-3.0-or-later
#
# The supervision tier (T7, docs/IMPLEMENTATION.md §2 and §4): the controller
# driven against real worker processes that really complete, are really killed,
# and are really orphaned -- the shipped binary itself (supervision_test.cpp),
# and the fake worker that misbehaves on demand (catalogue_test.cpp, the
# misbehaviour catalogue). A separate executable so the ledger's T7 location is
# a directory of tests and nothing else, and so these tests, which take seconds
# each, are a tier a contributor can run on its own.
add_executable(loadforge_supervision_tests supervision_test.cpp catalogue_test.cpp ladder_test.cpp
               telemetry_death_test.cpp control_test.cpp journal_kill_test.cpp)
target_link_libraries(loadforge_supervision_tests PRIVATE
  loadforge_cli loadforge_controller loadforge_platform loadforge_workloads_registry
  loadforge_workloads_mixed
  loadforge_verification_isolation GTest::gtest_main)
target_include_directories(loadforge_supervision_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# Two worker binaries: the real one, and the fake worker the misbehaviour
# catalogue runs (catalogue_test.cpp). Their paths are baked in rather than
# resolved from /proc/self/exe, which in a test process names the test.
target_compile_definitions(loadforge_supervision_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>"
  LOADFORGE_FAKE_WORKER="$<TARGET_FILE:loadforge_fake_worker>"
  LOADFORGE_FAULTY_WORKER="$<TARGET_FILE:loadforge_m2_faulty_worker>")
add_dependencies(loadforge_supervision_tests loadforge loadforge_fake_worker
                 loadforge_m2_faulty_worker)
loadforge_set_fp_contract(loadforge_supervision_tests)
loadforge_enable_sanitizers(loadforge_supervision_tests)
loadforge_enable_coverage(loadforge_supervision_tests)
target_compile_options(loadforge_supervision_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Workers are pinned to the first CPUs: two of these tests at once would share
# those CPUs and run each unit at half speed, which an instrumented build cannot
# afford (a unit longer than the stop grace is a worker killed mid-unit).
gtest_discover_tests(loadforge_supervision_tests
  PROPERTIES RESOURCE_LOCK loadforge_pinned_workers)

# The faulty worker (docs/IMPLEMENTATION.md §4, M2a exit criteria 2 and 4): the
# shipped worker around a compression workload that corrupts its output on
# purpose, the way LOADFORGE_FAULT says. The controller executes it as its
# worker and, for each rung of the isolation ladder, as the re-run, so every
# rung runs against really re-run workers (ladder_test.cpp). A test program
# only: nothing installs it and the shipped binary does not link it.
add_executable(loadforge_m2_faulty_worker faulty_worker.cpp)
target_link_libraries(loadforge_m2_faulty_worker PRIVATE loadforge_worker)
loadforge_set_warnings(loadforge_m2_faulty_worker)
loadforge_set_fp_contract(loadforge_m2_faulty_worker)
loadforge_enable_sanitizers(loadforge_m2_faulty_worker)
if(LOADFORGE_COVERAGE)
  target_link_options(loadforge_m2_faulty_worker PRIVATE --coverage)
endif()
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_supervision_tests PRIVATE -fprofile-update=atomic)
endif()
