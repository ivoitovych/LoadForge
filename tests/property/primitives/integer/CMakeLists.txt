# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/primitives/integer and nothing else.
loadforge_test_suite(loadforge_property_primitives_integer_tests
  SOURCES integer_property_test.cpp
  LINK loadforge_primitives_integer)

# Threads run the module's code under the coverage build; see src/primitives/integer.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_primitives_integer_tests PRIVATE -fprofile-update=atomic)
endif()
