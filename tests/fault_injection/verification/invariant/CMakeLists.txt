# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T5 and T5b tests of src/verification/invariant.
loadforge_test_suite(loadforge_fault_injection_verification_invariant_tests
  SOURCES invariant_corruption_test.cpp
  LINK loadforge_verification_invariant)
