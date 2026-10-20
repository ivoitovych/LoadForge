# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/oracle/adaptive_mesh.
#
# The oracle against OracleContract (good units built by a model that finds
# the mesh differently from the oracle, wrong units for every item it checks)
# and what the contract leaves to it: the sampled cells, the order and naming
# of the items, the tolerance held and breached at its edge, the hotspots it
# will not judge, its refusals. mesh_support.hpp is shared with this module's
# T3 and T5 suites.
loadforge_test_suite(loadforge_unit_verification_oracle_adaptive_mesh_tests
  SOURCES oracle_test.cpp
  LINK loadforge_verification_oracle_adaptive_mesh)

# check() runs on several threads here (the contract's concurrency test), and
# the header-only specification helpers it calls are instantiated in this unit:
# atomic counters, as for the library.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_unit_verification_oracle_adaptive_mesh_tests PRIVATE -fprofile-update=atomic)
endif()
