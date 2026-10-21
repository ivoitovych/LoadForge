# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T8 of src/controller/journal -- a writer really
# killed, a journal on a real file system replayed by another process.
loadforge_test_suite(loadforge_integration_controller_journal_tests
  SOURCES killed_writer_test.cpp
  LINK loadforge_controller_journal)
