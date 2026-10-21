# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_test_suite(loadforge_oracle_verification_oracle_monte_carlo_tests
  SOURCES monte_carlo_oracle_exact_test.cpp
  LINK loadforge_verification_oracle_monte_carlo)
target_include_directories(loadforge_oracle_verification_oracle_monte_carlo_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/unit/verification/oracle/monte_carlo")
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_oracle_verification_oracle_monte_carlo_tests PRIVATE
    -fprofile-update=atomic)
endif()
