# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/primitives/floating and nothing else.
loadforge_test_suite(loadforge_unit_primitives_floating_tests
  SOURCES floating_test.cpp
  LINK loadforge_primitives_floating)
