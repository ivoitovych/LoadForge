# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 of src/workloads/mixed -- the C++ schedules
# against a second implementation in Python (tests/fixtures/workloads/mixed).
loadforge_test_suite(loadforge_oracle_workloads_mixed_tests
  SOURCES oracle_test.cpp
  LINK loadforge_workloads_mixed)
