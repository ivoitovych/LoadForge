# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 of src/workloads/dense_linear and nothing else.
loadforge_test_suite(loadforge_oracle_workloads_dense_linear_tests
  SOURCES oracle_test.cpp
  LINK loadforge_workloads_dense_linear)
