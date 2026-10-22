# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/verification/integrity.
#
# The contract suites -- SliceRunnerContract against the runner, and the
# workload, oracle and executor suites against this stream's own doubles --
# the layout and comparison at their boundaries, and every step of every
# mapping the runner makes refused through the system-call seam.

# The doubles' out-of-line definitions, compiled once and linked by this
# suite and the fault-injection one: one object, one coverage record of the
# executor.hpp instantiations inside them (journal §1.22).
add_library(loadforge_unit_verification_integrity_support STATIC integrity_support.cpp)
target_link_libraries(loadforge_unit_verification_integrity_support
  PUBLIC loadforge_verification_integrity GTest::gtest)
target_include_directories(loadforge_unit_verification_integrity_support
  PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_compile_options(loadforge_unit_verification_integrity_support
  PRIVATE -Wall -Wextra -Wpedantic -Werror)
loadforge_set_fp_contract(loadforge_unit_verification_integrity_support)
loadforge_enable_sanitizers(loadforge_unit_verification_integrity_support)
loadforge_enable_coverage(loadforge_unit_verification_integrity_support)

loadforge_test_suite(loadforge_unit_verification_integrity_tests
  SOURCES
    contracts_test.cpp
    layout_test.cpp
    locking_test.cpp
    difference_test.cpp
    control_state_test.cpp
    runner_test.cpp
  LINK loadforge_unit_verification_integrity_support)
