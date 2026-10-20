# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 for src/workloads/compression and nothing else.
#
# BitExact enforced: at 1, 2, 3, 4 and 7 executor threads, across repeated
# runs and whatever scratch held, across configurations that must not touch
# the bits, and against the committed bytes on every architecture CI builds.
loadforge_test_suite(loadforge_determinism_workloads_compression_tests
  SOURCES
    bit_exact_test.cpp
  LINK loadforge_workloads_compression)

# Tasks run on several threads here, through executor.hpp's for_each_index
# instantiated in these units: atomic counters, as for the library
# (src/workloads/compression/CMakeLists.txt says why).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_determinism_workloads_compression_tests PRIVATE -fprofile-update=atomic)
endif()
