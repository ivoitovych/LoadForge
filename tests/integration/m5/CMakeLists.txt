# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of the M5 integration -- the shipped binary
# running the mixed workload and thermal and power cycling with a crash journal.
loadforge_test_suite(loadforge_integration_m5_tests
  SOURCES binary_test.cpp
  LINK loadforge_cli)
target_compile_definitions(loadforge_integration_m5_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_m5_tests loadforge)
