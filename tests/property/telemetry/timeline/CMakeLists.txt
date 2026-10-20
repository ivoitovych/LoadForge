# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/telemetry/timeline and nothing else.
loadforge_test_suite(loadforge_property_telemetry_timeline_tests
  SOURCES
    bounded_timeline_property_test.cpp
  LINK loadforge_telemetry_timeline)
