# SPDX-License-Identifier: GPL-3.0-or-later
#
# The supervision tier (T7, docs/IMPLEMENTATION.md §2 and §4): the controller
# driven against real worker processes -- the shipped binary itself -- that
# really complete, are really killed, and are really orphaned. A separate
# executable so the ledger's T7 location is a directory of tests and nothing
# else, and so these tests, which take seconds each, are a tier a contributor
# can run on its own.
add_executable(loadforge_supervision_tests supervision_test.cpp)
target_link_libraries(loadforge_supervision_tests PRIVATE
  loadforge_controller loadforge_platform GTest::gtest_main)
target_include_directories(loadforge_supervision_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# The worker binary is the real one; its path is baked in rather than
# resolved from /proc/self/exe, which in a test process names the test.
target_compile_definitions(loadforge_supervision_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_supervision_tests loadforge)
loadforge_set_fp_contract(loadforge_supervision_tests)
loadforge_enable_sanitizers(loadforge_supervision_tests)
loadforge_enable_coverage(loadforge_supervision_tests)
target_compile_options(loadforge_supervision_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

gtest_discover_tests(loadforge_supervision_tests)
