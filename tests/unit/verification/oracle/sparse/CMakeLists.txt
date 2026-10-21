# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the unit tier (T1) of src/verification/oracle/sparse.
loadforge_test_suite(loadforge_unit_verification_oracle_sparse_tests
  SOURCES sparse_oracle_unit_test.cpp
  LINK loadforge_verification_oracle_sparse)
