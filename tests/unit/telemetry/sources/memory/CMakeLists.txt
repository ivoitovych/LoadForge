# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the tests of src/telemetry/sources/memory and nothing else.
loadforge_test_suite(loadforge_unit_telemetry_sources_memory_tests
  SOURCES memory_test.cpp
  LINK loadforge_telemetry_sources_memory loadforge_telemetry_sources_thermal
       loadforge_topology loadforge_platform)
