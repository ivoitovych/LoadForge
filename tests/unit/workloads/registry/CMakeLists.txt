# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 for src/workloads/registry and nothing else.
loadforge_test_suite(loadforge_unit_workloads_registry_tests
  SOURCES
    registry_test.cpp
  LINK loadforge_workloads_registry)
