# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T10 soak of src/worker/executor -- millions of
# runs on one pool. A binary of its own, because it replaces the global
# allocation functions to count them, which no other test should inherit.
loadforge_test_suite(loadforge_soak_worker_executor_tests
  SOURCES soak_test.cpp
  LINK loadforge_worker_executor)
