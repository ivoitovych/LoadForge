# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of `loadforge selftest` -- the shipped binary checking itself.
loadforge_test_suite(loadforge_integration_selftest_tests
  SOURCES binary_test.cpp
  LINK loadforge_cli)
target_compile_definitions(loadforge_integration_selftest_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_selftest_tests loadforge)
