# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b of src/verification/oracle/compression.
#
# Corruption caught and classified -- every rule, and every single-bit flip of
# every unit the reference tool made -- and the checks that cannot be made
# reported as not made.
loadforge_test_suite(loadforge_fault_injection_verification_oracle_compression_tests
  SOURCES corruption_test.cpp
  LINK loadforge_verification_oracle_compression)
