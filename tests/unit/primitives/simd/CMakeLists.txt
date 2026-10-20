# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of the top level of src/primitives/simd.
loadforge_test_suite(loadforge_unit_primitives_simd_tests
  SOURCES
    select_path_test.cpp
  LINK loadforge_primitives_simd)
