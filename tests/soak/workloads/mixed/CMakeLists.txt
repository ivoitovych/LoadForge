# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T10 of src/workloads/mixed -- a worker rotated for
# as long as LOADFORGE_SOAK_SECONDS says, and a five-hour schedule walked whole.
loadforge_test_suite(loadforge_soak_workloads_mixed_tests
  SOURCES soak_test.cpp
  LINK loadforge_worker loadforge_verification_integrity loadforge_workloads_mixed
       loadforge_platform_fpenv loadforge_platform_isa)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_soak_workloads_mixed_tests PRIVATE -fprofile-update=atomic)
endif()
