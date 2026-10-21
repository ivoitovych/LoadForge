# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the tests of src/telemetry/sources/edac and nothing else.
loadforge_test_suite(loadforge_unit_telemetry_sources_edac_tests
  SOURCES edac_test.cpp
  LINK loadforge_telemetry_sources_edac loadforge_platform loadforge_topology)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_telemetry_sources_edac PRIVATE -fprofile-update=atomic)
  target_compile_options(loadforge_unit_telemetry_sources_edac_tests PRIVATE -fprofile-update=atomic)
endif()
