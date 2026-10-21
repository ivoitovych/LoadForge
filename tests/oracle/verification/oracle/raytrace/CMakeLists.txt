# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 of src/verification/oracle/raytrace.
#
# The oracle against geometry done another way: generated scenes, whose every
# pixel is solved again in extended precision from the quadratic and from the
# plane of each triangle, and scenes whose answer is known by hand.
loadforge_test_suite(loadforge_oracle_verification_oracle_raytrace_tests
  SOURCES geometry_test.cpp
  LINK loadforge_verification_oracle_raytrace)
