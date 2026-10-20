# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T5 and T5b tests of src/workloads/stencil.
loadforge_test_suite(loadforge_fault_injection_workloads_stencil_tests
  SOURCES
    stencil_fault_test.cpp
  LINK loadforge_workloads_stencil)
