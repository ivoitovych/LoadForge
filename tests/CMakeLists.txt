# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_provide_gtest()
loadforge_provide_tomlplusplus()
include(GoogleTest)

add_executable(loadforge_unit_tests
  unit/core/parse_error_test.cpp
  unit/core/result_test.cpp
  unit/core/duration_test.cpp
  unit/core/byte_size_test.cpp
  unit/main/cli_test.cpp
  unit/main/selftest_test.cpp
  unit/platform/syscall_error_test.cpp
  unit/platform/fs_test.cpp
  unit/platform/real_syscalls_test.cpp
  unit/platform/result_instantiations_test.cpp
  unit/third_party/tomlplusplus_smoke_test.cpp
)
target_link_libraries(loadforge_unit_tests PRIVATE
  loadforge_cli loadforge_platform GTest::gtest_main GTest::gmock loadforge::tomlplusplus)
# tests/ is on the include path so that support/ fakes resolve the same way
# src/ headers do, rather than by relative path from wherever a test lives.
target_include_directories(loadforge_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
loadforge_set_fp_contract(loadforge_unit_tests)
loadforge_enable_sanitizers(loadforge_unit_tests)
loadforge_enable_coverage(loadforge_unit_tests)
# Test code is product code, but gtest macros trip -Wold-style-cast and friends,
# so tests get the same warnings minus the ones the framework itself violates.
target_compile_options(loadforge_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

gtest_discover_tests(loadforge_unit_tests)

# Runs the real binary, so main() is genuinely covered rather than excluded.
add_test(NAME cli_smoke_version COMMAND loadforge --version)
add_test(NAME cli_smoke_help    COMMAND loadforge --help)
add_test(NAME cli_smoke_usage   COMMAND loadforge)
set_tests_properties(cli_smoke_version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.0\\.0-m0")
