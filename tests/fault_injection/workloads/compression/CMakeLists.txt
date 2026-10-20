# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b for src/workloads/compression and
# nothing else.
#
# T5: each way an output can be corrupted is caught by the test decoder, and
# the test says which check caught it and at which byte. T5b: evidence of
# the wrong size is reported as not checked, never as an agreement.
loadforge_test_suite(loadforge_fault_injection_workloads_compression_tests
  SOURCES
    corruption_test.cpp
    unrun_check_test.cpp
  LINK loadforge_workloads_compression)
