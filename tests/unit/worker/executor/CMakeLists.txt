# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/worker/executor.
#
# A suite of its own rather than sources added to loadforge_unit_tests: the
# pool's tests start real threads, and the mutation gate's binary is the M1
# modules' (tests/unit/resources/CMakeLists.txt says why that one joins it).
loadforge_test_suite(loadforge_unit_worker_executor_tests
  SOURCES
    thread_pool_test.cpp
    ordering_test.cpp
    fp_control_contract_test.cpp
  LINK loadforge_worker_executor)
