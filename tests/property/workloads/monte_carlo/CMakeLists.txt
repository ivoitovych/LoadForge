# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/workloads/monte_carlo and nothing else.
loadforge_test_suite(loadforge_property_workloads_monte_carlo_tests
  SOURCES estimate_test.cpp
  LINK loadforge_workloads_monte_carlo)
