# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/oracle/raytrace.
#
# The oracle against OracleContract (good units rendered by a model built
# differently from the oracle, wrong units for every item it checks) and what
# the contract leaves to it: the sampled pixels, the order and naming of the
# items, the ranges of the input, the malformed records, its refusals.
# scene_support.hpp is shared with this module's T3 and T5 suites.
loadforge_test_suite(loadforge_unit_verification_oracle_raytrace_tests
  SOURCES oracle_test.cpp
  LINK loadforge_verification_oracle_raytrace)
