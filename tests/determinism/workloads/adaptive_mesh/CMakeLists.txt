# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/workloads/adaptive_mesh and nothing else.
loadforge_test_suite(loadforge_determinism_workloads_adaptive_mesh_tests
  SOURCES determinism_test.cpp
  LINK loadforge_workloads_adaptive_mesh)
