# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/platform/isa.
loadforge_test_suite(loadforge_unit_platform_isa_tests
  SOURCES
    detect_test.cpp
    probe_test.cpp
  LINK loadforge_platform_isa)
