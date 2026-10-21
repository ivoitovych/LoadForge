# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T5 and T5b tests of src/verification/isolation.
#
# Faults injected into a modelled machine and the ladder's finding asserted;
# re-runs made impossible and the answer asserted never clean; every path of
# the ladder for both kinds of wrong result.
loadforge_test_suite(loadforge_fault_injection_verification_isolation_tests
  SOURCES
    ladder_fault_test.cpp
  LINK loadforge_verification_isolation)
