# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T8 tests of src/worker/executor -- the pool on
# the real kernel and the real floating-point registers.
loadforge_test_suite(loadforge_integration_worker_executor_tests
  SOURCES real_pool_test.cpp
  LINK loadforge_worker_executor)
# Tasks run concurrently on the pool's threads, and the header code they run
# (executor.hpp's for_each_index among it) is counted in this binary: atomic
# counters, for the reason src/worker/executor/CMakeLists.txt gives.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_integration_worker_executor_tests PRIVATE -fprofile-update=atomic)
endif()
