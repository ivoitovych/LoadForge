# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of src/reporters/csv.
loadforge_test_suite(loadforge_integration_reporters_csv_tests
  SOURCES csv_file_test.cpp
  LINK loadforge_reporters_csv loadforge_controller_results loadforge_controller)
