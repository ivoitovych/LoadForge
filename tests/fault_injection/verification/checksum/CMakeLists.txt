# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b of src/verification/checksum.
loadforge_test_suite(loadforge_fault_injection_verification_checksum_tests
  SOURCES
    classification_test.cpp
    seal_fault_test.cpp
  LINK loadforge_verification_checksum)
