# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T3 tests of src/verification/oracle/dense_linear,
# OracleContract among them.
loadforge_test_suite(loadforge_oracle_verification_oracle_dense_linear_tests
  SOURCES contract_test.cpp
  LINK loadforge_verification_oracle_dense_linear)
