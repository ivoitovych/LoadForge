# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the tests of src/telemetry/sources/frequency and nothing else.
loadforge_test_suite(loadforge_unit_telemetry_sources_frequency_tests
  SOURCES frequency_test.cpp
  LINK loadforge_telemetry_sources_frequency loadforge_telemetry_sources_thermal
       loadforge_topology loadforge_platform)
