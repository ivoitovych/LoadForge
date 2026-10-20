# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T4 of src/verification/checksum.
loadforge_test_suite(loadforge_property_verification_checksum_tests
  SOURCES
    compare_property_test.cpp
    crc64_property_test.cpp
  LINK loadforge_verification_checksum)
