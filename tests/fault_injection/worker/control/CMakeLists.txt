# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 of src/worker/control -- bytes that went wrong.
loadforge_test_suite(loadforge_fault_injection_worker_control_tests
  SOURCES flip_test.cpp
  LINK loadforge_worker_control)
