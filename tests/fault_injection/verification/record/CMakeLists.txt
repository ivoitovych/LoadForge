# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 of src/verification/record.
loadforge_test_suite(loadforge_fault_injection_verification_record_tests
  SOURCES
    decode_fuzz_test.cpp
  LINK loadforge_verification_record)
