# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 for src/workloads/raytrace and nothing else.
#
# BitExactFixedDecomposition: the same bytes for one tile edge at any thread
# count, on every path, on every run, and equal to the digests an independent
# model of the specification made (tests/fixtures/workloads/raytrace).
loadforge_test_suite(loadforge_determinism_workloads_raytrace_tests
  SOURCES
    determinism_test.cpp
  LINK loadforge_workloads_raytrace)

if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_determinism_workloads_raytrace_tests PRIVATE -fprofile-update=atomic)
endif()
