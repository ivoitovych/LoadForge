# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 tests of src/verification/isolation.
#
# IsolatorContract against the ladder, the ladder's own reading of the
# contract, and the choice of the other core over built sysfs trees and this
# machine's /sys.
loadforge_test_suite(loadforge_unit_verification_isolation_tests
  SOURCES
    ladder_test.cpp
    placement_test.cpp
  LINK loadforge_verification_isolation)
