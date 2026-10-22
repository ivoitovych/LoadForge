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
  unit/main/outcome_test.cpp
  unit/main/result_files_test.cpp
  unit/main/telemetry_rig_test.cpp
  unit/main/cli_telemetry_test.cpp
  unit/main/run_result_test.cpp
  unit/main/contract_mocks_test.cpp
  unit/controller/contract_mocks_test.cpp
  unit/controller/orchestration_telemetry_test.cpp
  unit/config/config_test.cpp
  unit/platform/syscall_error_test.cpp
  unit/platform/syscalls_defaults_test.cpp
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
  unit/topology/snapshot_test.cpp
  unit/worker/ipc/heartbeat_test.cpp
  unit/worker/ipc/record_test.cpp
  unit/worker/scheduler_test.cpp
  unit/worker/entry_test.cpp
  unit/worker/model_runner_test.cpp
  unit/worker/verified_run_test.cpp
  unit/worker/frames_test.cpp
  unit/worker/entry_workload_test.cpp
  unit/worker/model_codec_test.cpp
  unit/controller/consumer_models_test.cpp
  unit/controller/plan_test.cpp
  unit/controller/rerun_test.cpp
  unit/controller/isolate_test.cpp
  unit/controller/orchestration_verified_test.cpp
  unit/controller/supervision_test.cpp
  unit/controller/safety_test.cpp
  unit/controller/orchestration_test.cpp
  unit/reporters/console_test.cpp
  unit/platform/real_syscalls_test.cpp
  unit/platform/fake_syscalls_test.cpp
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
  LOADFORGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
# The CLI tests run the real binary as the workers of a real run.
add_dependencies(loadforge_unit_tests loadforge)
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
set_tests_properties(cli_smoke_version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.0\\.0-m2")

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
# The re-run switch through the real binary: misused, it refuses with its own
# code; used, it runs one unit on one CPU with every check on and exits clean.
add_test(NAME cli_rerun_bad_args_code
         COMMAND sh -c "'$<TARGET_FILE:loadforge>' --rerun --cpu 0; test $? -eq 22")
add_test(NAME cli_rerun_runs_a_unit_clean
         COMMAND sh -c "'$<TARGET_FILE:loadforge>' --rerun --workload compression --seed 5 --footprint-bytes 1048576 --threads 1 --decomposition 0 --isa scalar --lock try --line-bytes 64 --version 1 --iteration 7 --cpu 0; test $? -eq 0")
set_tests_properties(cli_worker_bad_args_message PROPERTIES
                     PASS_REGULAR_EXPRESSION "loadforge --worker: --workers: required")

# M1 exit criterion 1 through the real binary: the controller runs the null
# workload in two worker processes for the configured second, honouring its
# limits, and says so. A mode this build does not run is refused by name.
add_test(NAME cli_stress_runs
         COMMAND loadforge stress --config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/short-stress.toml")
set_tests_properties(cli_stress_runs PROPERTIES
                     PASS_REGULAR_EXPRESSION "2 of 2 worker\\(s\\) completed")
# The topology snapshot through the real binary on the real machine: taken
# before the first fork and printed in the banner, which precedes the spawn.
add_test(NAME cli_stress_describes_the_machine
         COMMAND loadforge stress --config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/short-stress.toml")
set_tests_properties(cli_stress_describes_the_machine PROPERTIES
                     PASS_REGULAR_EXPRESSION "\n  topology: [0-9]+ logical CPU\\(s\\) on [0-9]+ core\\(s\\) in [0-9]+ package\\(s\\); [^\n]*\n  limit duration")
add_test(NAME cli_stress_refuses_other_modes
         COMMAND loadforge stress --config "${CMAKE_SOURCE_DIR}/tests/fixtures/config/benchmark-mode.toml")
set_tests_properties(cli_stress_refuses_other_modes PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_stress_usage COMMAND loadforge stress)
set_tests_properties(cli_stress_usage PROPERTIES WILL_FAIL TRUE)
# M2a exit criterion 1 and 5 and M2b exit criterion 1 through the real binary:
# each workload is stressed, checked by its oracle, and the run says what the
# machine can and cannot be read for before it starts.
# A worker sees the stop request only between units and a verified unit can take
# 1.5 s under a sanitizer, so the controller gives a verifying run ten seconds
# to stop (kVerifyingStopGraceNs in src/main/cli.cpp). The run is two seconds
# long; a longer one only hid the one-second grace (2026-10-22, #95's TSan job).
foreach(workload IN ITEMS compression dense_linear)
  add_test(NAME cli_stress_test_${workload}
           COMMAND loadforge stress --test ${workload} --duration 2s --workers 2 --footprint 2MiB)
  set_tests_properties(cli_stress_test_${workload} PROPERTIES
                       RESOURCE_LOCK loadforge_pinned_workers
                       PASS_REGULAR_EXPRESSION
                       "2 of 2 worker\\(s\\) completed.*the oracle checked [1-9][0-9]* of them over [1-9][0-9]* sampled block\\(s\\) and found 0 wrong")
endforeach()
add_test(NAME cli_stress_test_states_the_capabilities_first
         COMMAND loadforge stress --test compression --duration 1s --workers 2 --footprint 6MiB)
set_tests_properties(cli_stress_test_states_the_capabilities_first PROPERTIES
                     PASS_REGULAR_EXPRESSION
                     "telemetry capability: [0-9]+ of [0-9]+ metric\\(s\\) present.*spawned 2 worker")
add_test(NAME cli_stress_test_unknown_workload COMMAND loadforge stress --test no_such)
set_tests_properties(cli_stress_test_unknown_workload PROPERTIES WILL_FAIL TRUE)

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
