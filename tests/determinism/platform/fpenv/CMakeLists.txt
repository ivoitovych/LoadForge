# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T6 tests of src/platform/fpenv.
loadforge_test_suite(loadforge_determinism_platform_fpenv_tests
  SOURCES
    subnormal_test.cpp
  LINK loadforge_platform_fpenv)
find_package(Threads REQUIRED)
target_link_libraries(loadforge_determinism_platform_fpenv_tests PRIVATE Threads::Threads)
