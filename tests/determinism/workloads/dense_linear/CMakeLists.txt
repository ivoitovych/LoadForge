# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/workloads/dense_linear and nothing else.
loadforge_test_suite(loadforge_determinism_workloads_dense_linear_tests
  SOURCES determinism_test.cpp
  LINK loadforge_workloads_dense_linear)
