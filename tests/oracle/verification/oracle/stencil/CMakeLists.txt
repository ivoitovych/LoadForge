# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the oracle tier (T3) of src/verification/oracle/stencil.
loadforge_test_suite(loadforge_oracle_verification_oracle_stencil_tests
  SOURCES stencil_oracle_test.cpp
  LINK loadforge_verification_oracle_stencil)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_oracle_verification_oracle_stencil_tests
                         PRIVATE -fprofile-update=atomic)
endif()
