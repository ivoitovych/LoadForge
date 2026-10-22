# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of the M6 integration -- the trial runner and
# `loadforge explore` over the real controller, real worker processes and this
# machine's own /proc and /sys.
loadforge_test_suite(loadforge_integration_m6_tests
  SOURCES runner_test.cpp binary_test.cpp
  LINK loadforge_cli)
target_compile_definitions(loadforge_integration_m6_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_m6_tests loadforge)
