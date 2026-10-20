# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the determinism tests (T6) of src/workloads/fft and nothing else.
loadforge_test_suite(loadforge_determinism_workloads_fft_tests
  SOURCES determinism_test.cpp
  LINK loadforge_workloads_fft)
