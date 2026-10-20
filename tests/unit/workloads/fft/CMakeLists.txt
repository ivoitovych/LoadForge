# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the unit tests (T1, T2) of src/workloads/fft and nothing else.
loadforge_test_suite(loadforge_unit_workloads_fft_tests
  SOURCES
    twiddle_test.cpp
    kernel_test.cpp
    workload_test.cpp
    contract_test.cpp
  LINK loadforge_workloads_fft)
