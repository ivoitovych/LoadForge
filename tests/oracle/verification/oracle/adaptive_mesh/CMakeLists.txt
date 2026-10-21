# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 of src/verification/oracle/adaptive_mesh.
#
# The oracle against arithmetic done another way: the exact sum of a coarse
# cell's leaves, held in fixed point, against which the specification's bound
# and the oracle's tolerance are checked on generated units, and mesh counts
# known by hand.
loadforge_test_suite(loadforge_oracle_verification_oracle_adaptive_mesh_tests
  SOURCES exact_sum_test.cpp
  LINK loadforge_verification_oracle_adaptive_mesh)
