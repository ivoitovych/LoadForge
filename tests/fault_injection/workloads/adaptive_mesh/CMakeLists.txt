# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 of src/workloads/adaptive_mesh and nothing else.
loadforge_test_suite(loadforge_fault_injection_workloads_adaptive_mesh_tests
  SOURCES fault_test.cpp
  LINK loadforge_workloads_adaptive_mesh)
