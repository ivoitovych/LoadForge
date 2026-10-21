# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T3 tests of src/verification/residual -- the
# derived bound against exact arithmetic on the specification's inputs.
loadforge_test_suite(loadforge_oracle_verification_residual_tests
  SOURCES bound_test.cpp
  LINK loadforge_verification_residual)
