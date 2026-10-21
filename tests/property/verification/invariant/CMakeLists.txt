# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/verification/invariant.
loadforge_test_suite(loadforge_property_verification_invariant_tests
  SOURCES invariant_property_test.cpp
  LINK loadforge_verification_invariant)
