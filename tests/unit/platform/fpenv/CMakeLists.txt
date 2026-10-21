# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/platform/fpenv.
loadforge_test_suite(loadforge_unit_platform_fpenv_tests
  SOURCES
    check_test.cpp
    registers_test.cpp
    real_control_test.cpp
    environment_test.cpp
  LINK loadforge_platform_fpenv)
