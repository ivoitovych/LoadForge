# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/checksum.
loadforge_test_suite(loadforge_unit_verification_checksum_tests
  SOURCES
    compare_test.cpp
    crc64_test.cpp
    seal_test.cpp
  LINK loadforge_verification_checksum)
