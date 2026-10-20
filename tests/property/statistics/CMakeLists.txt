# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/statistics and nothing else.
loadforge_test_suite(loadforge_property_statistics_tests
  SOURCES metamorphic_test.cpp
  LINK loadforge_statistics)
