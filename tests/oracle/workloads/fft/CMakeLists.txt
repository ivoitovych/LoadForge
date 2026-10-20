# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the oracle tests (T3) of src/workloads/fft and nothing else.
#
# fft_support.hpp in this directory is shared with the other tiers of this
# module (property, fault_injection, determinism, unit): the independent direct
# DFT and the checker written from spec.hpp alone.
loadforge_test_suite(loadforge_oracle_workloads_fft_tests
  SOURCES oracle_test.cpp
  LINK loadforge_workloads_fft)
