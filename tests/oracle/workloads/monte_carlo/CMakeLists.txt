# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T3 tests of src/workloads/monte_carlo and nothing else.
loadforge_test_suite(loadforge_oracle_workloads_monte_carlo_tests
  SOURCES replay_test.cpp
  LINK loadforge_workloads_monte_carlo)
