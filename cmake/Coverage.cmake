# SPDX-License-Identifier: GPL-3.0-or-later
#
# Coverage instrumentation. The gate itself (100% line and branch) lives in
# tools/coverage.sh, which invokes gcovr with the exclusions documented in
# docs/PLAN.md §7.3 -- throw and unreachable branches are excluded so that the
# number describes the code's own decisions rather than compiler-generated
# exception edges.

function(loadforge_enable_coverage target)
  if(NOT LOADFORGE_COVERAGE)
    return()
  endif()
  # -fprofile-update=atomic: plain gcov counters are incremented without
  # synchronisation, so code that tests run on several threads (the executor,
  # the oracles' concurrency tests, the workloads' thread-count tests) loses
  # increments, and the arithmetic gcov does over them can produce negative
  # branch counts, which gcovr refuses outright (GCC bug 68080). It turned
  # the coverage job red on a race, not on a coverage gap. Wave 1's streams
  # each set it on their own targets; it belongs here, for every target.
  target_compile_options(${target} PRIVATE --coverage -fprofile-update=atomic -O0 -g -fno-inline)
  target_link_options(${target} PRIVATE --coverage)
endfunction()
