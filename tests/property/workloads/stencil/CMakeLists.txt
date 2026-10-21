# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/workloads/stencil.
loadforge_test_suite(loadforge_property_workloads_stencil_tests
  SOURCES
    stencil_property_test.cpp
  LINK loadforge_workloads_stencil)
