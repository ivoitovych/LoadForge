# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/verification/integrity.
#
# The contract suites -- SliceRunnerContract against the runner, and the
# workload, oracle and executor suites against this stream's own doubles --
# the layout and comparison at their boundaries, and every step of every
# mapping the runner makes refused through the system-call seam.
loadforge_test_suite(loadforge_unit_verification_integrity_tests
  SOURCES
    contracts_test.cpp
    layout_test.cpp
    difference_test.cpp
    control_state_test.cpp
    runner_test.cpp
  LINK loadforge_verification_integrity)
