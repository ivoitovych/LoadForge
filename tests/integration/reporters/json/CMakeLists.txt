# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of src/reporters/json.
loadforge_test_suite(loadforge_integration_reporters_json_tests
  SOURCES json_file_test.cpp
  LINK loadforge_reporters_json loadforge_controller_results loadforge_controller)
