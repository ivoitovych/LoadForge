# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T6 tests of src/primitives/cache and nothing else.
loadforge_test_suite(loadforge_determinism_primitives_cache_tests
  SOURCES cache_determinism_test.cpp
  LINK loadforge_primitives_cache)
