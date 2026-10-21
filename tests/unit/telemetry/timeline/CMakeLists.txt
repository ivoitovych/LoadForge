# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/telemetry/timeline and nothing else.
loadforge_test_suite(loadforge_unit_telemetry_timeline_tests
  SOURCES
    bounded_timeline_test.cpp
  LINK loadforge_telemetry_timeline)
