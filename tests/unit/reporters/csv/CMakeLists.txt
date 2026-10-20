# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T1 of src/reporters/csv.
loadforge_test_suite(loadforge_unit_reporters_csv_tests
  SOURCES csv_text_test.cpp csv_writer_test.cpp
  LINK loadforge_reporters_csv loadforge_controller_results loadforge_controller)
