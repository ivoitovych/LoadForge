# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/verification/golden.
loadforge_test_suite(loadforge_determinism_verification_golden_tests
  SOURCES
    golden_determinism_test.cpp
  LINK loadforge_verification_golden)
