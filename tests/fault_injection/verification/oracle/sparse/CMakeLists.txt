# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the fault-injection tier (T5, T5b) of
# src/verification/oracle/sparse.
loadforge_test_suite(loadforge_fault_injection_verification_oracle_sparse_tests
  SOURCES sparse_fault_injection_test.cpp
  LINK loadforge_verification_oracle_sparse)
