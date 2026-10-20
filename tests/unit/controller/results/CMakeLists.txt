# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T1 and T2 of src/controller/results.
loadforge_test_suite(loadforge_unit_controller_results_tests
  SOURCES
    verdict_test.cpp
    aggregate_test.cpp
    fingerprint_test.cpp
  LINK loadforge_controller_results loadforge_controller)
