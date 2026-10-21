# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/verification/oracle/dense_linear.
loadforge_test_suite(loadforge_unit_verification_oracle_dense_linear_tests
  SOURCES oracle_test.cpp
  LINK loadforge_verification_oracle_dense_linear)
