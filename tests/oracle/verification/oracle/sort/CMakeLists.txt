# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_test_suite(loadforge_oracle_verification_oracle_sort_tests
  SOURCES sort_oracle_differential_test.cpp
  LINK loadforge_verification_oracle_sort)
target_include_directories(loadforge_oracle_verification_oracle_sort_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/unit/verification/oracle/sort")
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_oracle_verification_oracle_sort_tests PRIVATE
    -fprofile-update=atomic)
endif()
