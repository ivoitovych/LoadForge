# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/golden, and WorkloadContract
# run against this module's own mock workload (ADR 0003).
loadforge_test_suite(loadforge_unit_verification_golden_tests
  SOURCES
    golden_compare_test.cpp
    golden_file_test.cpp
    workload_contract_test.cpp
  LINK loadforge_verification_golden)
