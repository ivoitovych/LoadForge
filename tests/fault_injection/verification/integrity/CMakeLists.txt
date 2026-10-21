# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T5 and T5b tests of src/verification/integrity.
#
# Corruption injected into the memory the runner holds -- its slots and the
# copies of its control state -- with the record each one must produce
# asserted; and every way a check can be prevented, each a
# VerificationNotPerformed and never a wrong result.
loadforge_test_suite(loadforge_fault_injection_verification_integrity_tests
  SOURCES
    bracket_test.cpp
    unrun_test.cpp
    control_state_fault_test.cpp
  LINK loadforge_unit_verification_integrity_support)
