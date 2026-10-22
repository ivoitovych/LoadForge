# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/workloads/mixed -- the same schedule on
# every run, on every thread and on every architecture.
loadforge_test_suite(loadforge_determinism_workloads_mixed_tests
  SOURCES determinism_test.cpp
  LINK loadforge_workloads_mixed)
