# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T5 and T5b tests of src/workloads/monte_carlo and nothing else.
loadforge_test_suite(loadforge_fault_injection_workloads_monte_carlo_tests
  SOURCES corruption_test.cpp
  LINK loadforge_workloads_monte_carlo)
