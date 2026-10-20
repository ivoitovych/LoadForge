# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T10 soak of src/worker/executor -- millions of
# runs on one pool. A binary of its own, because it replaces the global
# allocation functions to count them, which no other test should inherit.
loadforge_test_suite(loadforge_soak_worker_executor_tests
  SOURCES soak_test.cpp
  LINK loadforge_worker_executor)
# Tasks run concurrently on the pool's threads, and the header code they run
# (executor.hpp's for_each_index among it) is counted in this binary: atomic
# counters, for the reason src/worker/executor/CMakeLists.txt gives.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_soak_worker_executor_tests PRIVATE -fprofile-update=atomic)
endif()
