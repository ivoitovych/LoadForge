# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the tests of src/telemetry/capability and nothing else.
#
# The sources are linked for the real-against-real tier only: the report built
# from the four real sources over the captured machine, and over this one.
loadforge_test_suite(loadforge_unit_telemetry_capability_tests
  SOURCES capability_test.cpp
  LINK loadforge_telemetry_capability
       loadforge_telemetry_sources_thermal loadforge_telemetry_sources_fan
       loadforge_telemetry_sources_frequency loadforge_telemetry_sources_memory
       loadforge_topology loadforge_platform)
