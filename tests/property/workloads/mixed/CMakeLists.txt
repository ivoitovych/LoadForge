# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T4 of src/workloads/mixed -- what every schedule
# the options can make keeps true.
loadforge_test_suite(loadforge_property_workloads_mixed_tests
  SOURCES property_test.cpp
  LINK loadforge_workloads_mixed)
