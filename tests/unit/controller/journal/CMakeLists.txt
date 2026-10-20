# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 and T2 of src/controller/journal. Single
# threaded, so no atomic profile updates are needed.
loadforge_test_suite(loadforge_unit_controller_journal_tests
  SOURCES
    format_test.cpp
    journal_test.cpp
    file_storage_test.cpp
    file_ops_test.cpp
  LINK loadforge_controller_journal)
