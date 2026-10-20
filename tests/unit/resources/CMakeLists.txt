# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the tests of src/resources and nothing else.
#
# These join loadforge_unit_tests rather than building an executable of their
# own (loadforge_test_suite), because that binary is the one the mutation gate
# runs (.github/workflows/mutation.yml), and M1 is complete only when its first
# sweep reports src/resources clean with the rest. When the gate runs every
# test executable (stream 11), this fragment can become a suite of its own.
target_sources(loadforge_unit_tests PRIVATE
  cgroup_test.cpp
  memory_budget_test.cpp
  cgroup_oom_record_test.cpp)
target_link_libraries(loadforge_unit_tests PRIVATE loadforge_resources)

# The memory budget through the real binary on the real machine: a run that
# configures memory must find the budget this machine offers and say the limit
# is enforced. NOT ENFORCED here would mean the reader refused a real machine,
# which is the failure a reader of /proc and the cgroup files is most likely
# to have and least likely to show anywhere else.
add_test(NAME cli_stress_enforces_memory
         COMMAND loadforge stress --config "${PROJECT_SOURCE_DIR}/tests/fixtures/config/short-stress-memory.toml")
set_tests_properties(cli_stress_enforces_memory PROPERTIES
                     PASS_REGULAR_EXPRESSION "limit memory: enforced \\(checked at start against a ceiling of")
