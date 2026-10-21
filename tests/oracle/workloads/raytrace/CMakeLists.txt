# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 for src/workloads/raytrace and nothing else.
#
# The workload against brute force: every primitive tried for every pixel,
# for sampled units, every tile edge and every path this machine can run, and
# for inputs a bit flipped.
loadforge_test_suite(loadforge_oracle_workloads_raytrace_tests
  SOURCES
    brute_force_test.cpp
  LINK loadforge_workloads_raytrace)

if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_oracle_workloads_raytrace_tests PRIVATE -fprofile-update=atomic)
endif()
