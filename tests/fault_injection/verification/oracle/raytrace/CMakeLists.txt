# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b of src/verification/oracle/raytrace.
#
# Corruption caught and classified -- every bit of every sampled pixel and of
# every tile sum, and the bits of pixels that are only seen through a tile sum
# -- and the checks that cannot be made reported as not made.
loadforge_test_suite(loadforge_fault_injection_verification_oracle_raytrace_tests
  SOURCES corruption_test.cpp
  LINK loadforge_verification_oracle_raytrace)
