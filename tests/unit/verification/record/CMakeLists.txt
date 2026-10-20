# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/record.
loadforge_test_suite(loadforge_unit_verification_record_tests
  SOURCES
    codec_contract_test.cpp
    codec_test.cpp
    format_test.cpp
    render_test.cpp
  LINK loadforge_verification_record)
