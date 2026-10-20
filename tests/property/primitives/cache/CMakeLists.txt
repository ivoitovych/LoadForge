# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/primitives/cache and nothing else.
loadforge_test_suite(loadforge_property_primitives_cache_tests
  SOURCES cache_property_test.cpp
  LINK loadforge_primitives_cache)
