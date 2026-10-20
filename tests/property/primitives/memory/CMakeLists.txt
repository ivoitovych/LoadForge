# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T4 tests of src/primitives/memory and nothing else.
loadforge_test_suite(loadforge_property_primitives_memory_tests
  SOURCES memory_property_test.cpp
  LINK loadforge_primitives_memory)

# Threads run the module's code under the coverage build; see src/primitives/memory.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_primitives_memory_tests PRIVATE -fprofile-update=atomic)
endif()
