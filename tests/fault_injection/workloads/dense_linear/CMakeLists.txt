# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 of src/workloads/dense_linear and nothing else.
loadforge_test_suite(loadforge_fault_injection_workloads_dense_linear_tests
  SOURCES fault_test.cpp
  LINK loadforge_workloads_dense_linear)
