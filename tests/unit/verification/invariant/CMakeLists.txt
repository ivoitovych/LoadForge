# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T5b tests of src/verification/invariant.
loadforge_test_suite(loadforge_unit_verification_invariant_tests
  SOURCES invariant_test.cpp
  LINK loadforge_verification_invariant)
