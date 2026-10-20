# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of src/controller/results.
loadforge_test_suite(loadforge_integration_controller_results_tests
  SOURCES fingerprint_machine_test.cpp
  LINK loadforge_controller_results loadforge_controller)
