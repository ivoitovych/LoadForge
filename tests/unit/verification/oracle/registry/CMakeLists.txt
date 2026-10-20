# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 for src/verification/oracle/registry and
# nothing else. It links the workload registry as well, to read the plans the
# oracles are created for: the contract's own words, "the plan the workload
# resolved from the same configuration".
loadforge_test_suite(loadforge_unit_verification_oracle_registry_tests
  SOURCES
    registry_test.cpp
  LINK loadforge_verification_oracle_registry loadforge_workloads_registry)
