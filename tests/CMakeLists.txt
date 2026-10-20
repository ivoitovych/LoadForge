# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_provide_gtest()
include(GoogleTest)

add_executable(loadforge_unit_tests
  unit/core/parse_error_test.cpp
  unit/core/result_test.cpp
  unit/core/duration_test.cpp
  unit/core/byte_size_test.cpp
  unit/core/crc32_test.cpp
  unit/main/cli_test.cpp
  unit/main/selftest_test.cpp
  unit/config/config_test.cpp
  unit/platform/syscall_error_test.cpp
  unit/platform/fs_test.cpp
  unit/platform/exit_status_test.cpp
  unit/platform/process_test.cpp
  unit/platform/worker_pool_test.cpp
  unit/platform/clock_test.cpp
  unit/platform/affinity_test.cpp
  unit/platform/memory_test.cpp
  unit/topology/cpu_list_test.cpp
  unit/topology/source_test.cpp
  unit/topology/cpu_topology_test.cpp
  unit/topology/cache_test.cpp
  unit/topology/numa_test.cpp
  unit/worker/ipc/heartbeat_test.cpp
  unit/worker/ipc/record_test.cpp
  unit/worker/scheduler_test.cpp
  unit/worker/entry_test.cpp
  unit/controller/supervision_test.cpp
  unit/controller/safety_test.cpp
  unit/controller/orchestration_test.cpp
  unit/reporters/console_test.cpp
  unit/platform/real_syscalls_test.cpp
  unit/platform/result_instantiations_test.cpp
  unit/third_party/tomlplusplus_smoke_test.cpp
)
target_link_libraries(loadforge_unit_tests PRIVATE
  loadforge_cli loadforge_config loadforge_platform loadforge_topology loadforge_worker
  loadforge_controller GTest::gtest_main GTest::gmock loadforge::tomlplusplus)
# tests/ is on the include path so that support/ fakes resolve the same way
# src/ headers do, rather than by relative path from wherever a test lives.
target_include_directories(loadforge_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Real files under the source tree for the config tests: the shipped quick
# config and the refused fixtures, by absolute path so ctest's working
# directory does not matter.
target_compile_definitions(loadforge_unit_tests PRIVATE
  LOADFORGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
loadforge_set_fp_contract(loadforge_unit_tests)
loadforge_enable_sanitizers(loadforge_unit_tests)
loadforge_enable_coverage(loadforge_unit_tests)
# Test code is product code, but gtest macros trip -Wold-style-cast and friends,
# so tests get the same warnings minus the ones the framework itself violates.
target_compile_options(loadforge_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

gtest_discover_tests(loadforge_unit_tests)

# Runs the real binary, so main() is genuinely covered rather than excluded.
add_test(NAME cli_smoke_version COMMAND loadforge --version)
add_test(NAME cli_smoke_help    COMMAND loadforge --help)
add_test(NAME cli_smoke_usage   COMMAND loadforge)
set_tests_properties(cli_smoke_version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.0\\.0-m0")

# The config module's T8: the real binary reads real files. One that parses,
# one that does not exist, one that is refused by the schema -- and the refused
# one must fail, not print a message and exit 0, which is what WILL_FAIL checks.
add_test(NAME cli_check_config_ok
         COMMAND loadforge --check-config "${CMAKE_SOURCE_DIR}/config/quick.toml")
set_tests_properties(cli_check_config_ok PROPERTIES PASS_REGULAR_EXPRESSION "^ok: ")
add_test(NAME cli_check_config_missing
         COMMAND loadforge --check-config "${CMAKE_SOURCE_DIR}/config/no-such-file.toml")
set_tests_properties(cli_check_config_missing PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_check_config_refused
         COMMAND loadforge --check-config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/unknown-key.toml")
set_tests_properties(cli_check_config_refused PROPERTIES WILL_FAIL TRUE)

# The worker's internal switch, through the real binary: misused, it must
# refuse with the bad-arguments code and name the argument. The worker run
# for real, with a region and pipes, is driven in-process by the CLI tests
# and by the supervision tier once the controller exists.
add_test(NAME cli_worker_bad_args_code
         COMMAND sh -c "'$<TARGET_FILE:loadforge>' --worker --slot 0; test $? -eq 125")
add_test(NAME cli_worker_bad_args_message COMMAND loadforge --worker --slot 0)
set_tests_properties(cli_worker_bad_args_message PROPERTIES
                     PASS_REGULAR_EXPRESSION "loadforge --worker: --workers: required")

# M1 exit criterion 1 through the real binary: the controller runs the null
# workload in two worker processes for the configured second, honouring its
# limits, and says so. A mode this build does not run is refused by name.
add_test(NAME cli_stress_runs
         COMMAND loadforge stress --config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/short-stress.toml")
set_tests_properties(cli_stress_runs PROPERTIES
                     PASS_REGULAR_EXPRESSION "2 of 2 worker\\(s\\) completed")
add_test(NAME cli_stress_refuses_other_modes
         COMMAND loadforge stress --config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/benchmark-mode.toml")
set_tests_properties(cli_stress_refuses_other_modes PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_stress_usage COMMAND loadforge stress)
set_tests_properties(cli_stress_usage PROPERTIES WILL_FAIL TRUE)

# The fake worker the supervision tier drives the controller against.
add_subdirectory(support/fake_worker)

# The test-hook gate against this build's own binaries: the shipped one passes,
# and the fake worker -- the thing the gate exists to keep out -- is refused by
# name. tests/tools/test_tooling.sh has the gate's own cases; these are the
# real artifacts, under whatever preset is building them.
add_test(NAME release_gate_admits_the_shipped_binary
         COMMAND "${CMAKE_SOURCE_DIR}/tools/check-no-test-hooks.sh" "$<TARGET_FILE:loadforge>")
set_tests_properties(release_gate_admits_the_shipped_binary PROPERTIES
                     PASS_REGULAR_EXPRESSION "0 test hook\\(s\\), ok")
add_test(NAME release_gate_refuses_the_fake_worker
         COMMAND "${CMAKE_SOURCE_DIR}/tools/check-no-test-hooks.sh"
                 "$<TARGET_FILE:loadforge_fake_worker>")
set_tests_properties(release_gate_refuses_the_fake_worker PROPERTIES
                     PASS_REGULAR_EXPRESSION "TEST HOOK: .*loadforge::fake_worker::")

# T7: the supervision tier, with really spawned, really killed children.
add_subdirectory(supervision)

# Every other CMakeLists.txt under tests/ is a component's test fragment --
# tests/<tier>/<component>/CMakeLists.txt -- and the contracts' own suites in
# tests/contracts/. Found by glob, so no stream edits this file to add its
# tests (cmake/Components.cmake, ADR 0004). The two subdirectories above are
# added explicitly; fixtures and the gates' own tests are never build fragments.
loadforge_add_fragments("${CMAKE_CURRENT_SOURCE_DIR}"
  EXCLUDE supervision/CMakeLists.txt support/fake_worker/CMakeLists.txt
  EXCLUDE_TREES fixtures tools)
