# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b of src/verification/oracle/adaptive_mesh.
#
# Corruption caught and classified -- every bit of every leaf count and of
# every sampled integral, each judged against the tolerance the test derives
# for itself -- and the checks that cannot be made reported as not made.
loadforge_test_suite(loadforge_fault_injection_verification_oracle_adaptive_mesh_tests
  SOURCES corruption_test.cpp
  LINK loadforge_verification_oracle_adaptive_mesh)
