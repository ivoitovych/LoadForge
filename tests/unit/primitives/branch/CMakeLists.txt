# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/primitives/branch and nothing else.
loadforge_test_suite(loadforge_unit_primitives_branch_tests
  SOURCES branch_test.cpp
  LINK loadforge_primitives_branch)
