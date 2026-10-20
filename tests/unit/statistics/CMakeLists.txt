# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/statistics and nothing else.
loadforge_test_suite(loadforge_unit_statistics_tests
  SOURCES summarizer_test.cpp fixture_test.cpp
  LINK loadforge_statistics)
