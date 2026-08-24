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
  target_compile_options(${target} PRIVATE --coverage -O0 -g -fno-inline)
  target_link_options(${target} PRIVATE --coverage)
endfunction()
