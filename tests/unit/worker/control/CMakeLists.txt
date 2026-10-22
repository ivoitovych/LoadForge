# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 and T5 of src/worker/control -- the codec, run
# against the contract's suite, pinned bytes and every refusal.
loadforge_test_suite(loadforge_unit_worker_control_tests
  SOURCES control_test.cpp
  LINK loadforge_worker_control)
