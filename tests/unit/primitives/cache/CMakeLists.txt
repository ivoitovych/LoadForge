# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/primitives/cache and nothing else.
loadforge_test_suite(loadforge_unit_primitives_cache_tests
  SOURCES cache_test.cpp
  LINK loadforge_primitives_cache loadforge_platform)
