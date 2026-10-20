# SPDX-License-Identifier: GPL-3.0-or-later
# T4 for src/explore (ADR 0004): properties of the search under a mocked runner.
loadforge_test_suite(loadforge_property_explore_tests
  SOURCES
    search_property_test.cpp
  LINK loadforge_explore loadforge_topology loadforge_controller)
