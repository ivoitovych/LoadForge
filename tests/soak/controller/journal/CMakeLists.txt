# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T10 of src/controller/journal -- hours of appends
# at the real interval, and nothing grows but the file.
loadforge_test_suite(loadforge_soak_controller_journal_tests
  SOURCES journal_soak_test.cpp
  LINK loadforge_controller_journal)
