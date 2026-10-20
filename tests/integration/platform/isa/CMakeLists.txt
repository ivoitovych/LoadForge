# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T8 test of src/platform/isa -- the real machine.
loadforge_test_suite(loadforge_integration_platform_isa_tests
  SOURCES
    machine_test.cpp
  LINK loadforge_platform_isa)
