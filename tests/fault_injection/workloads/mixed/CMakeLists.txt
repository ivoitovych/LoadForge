# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b of src/workloads/mixed -- a fault that
# arrives after a rotation, and the check that could not be made across one. The
# worker's loop (src/worker) runs for real; the faults are injected into the
# workloads it is made to run.
loadforge_test_suite(loadforge_fault_injection_workloads_mixed_tests
  SOURCES rotation_faults_test.cpp
  LINK loadforge_worker loadforge_verification_integrity loadforge_workloads_mixed
       loadforge_platform_fpenv loadforge_platform_isa)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_fault_injection_workloads_mixed_tests PRIVATE
                         -fprofile-update=atomic)
endif()
