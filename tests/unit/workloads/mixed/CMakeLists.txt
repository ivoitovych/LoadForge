# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/workloads/mixed -- the schedules, their
# validation, the roster, and the registry contract run against this module's
# own mock of it.
loadforge_test_suite(loadforge_unit_workloads_mixed_tests
  SOURCES
    schedule_test.cpp
    mixed_test.cpp
    cycling_test.cpp
    roster_test.cpp
  LINK loadforge_workloads_mixed)
