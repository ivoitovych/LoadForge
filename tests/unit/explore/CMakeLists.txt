# SPDX-License-Identifier: GPL-3.0-or-later
# T1 for src/explore (ADR 0004).
loadforge_test_suite(loadforge_unit_explore_tests
  SOURCES
    estimate_test.cpp
    drift_test.cpp
    objective_test.cpp
    search_test.cpp
    runner_contract_test.cpp
  LINK loadforge_explore loadforge_topology loadforge_controller)
