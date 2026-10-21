# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/verification/residual.
loadforge_test_suite(loadforge_unit_verification_residual_tests
  SOURCES residual_test.cpp
  LINK loadforge_verification_residual)
