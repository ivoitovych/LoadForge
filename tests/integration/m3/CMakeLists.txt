# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of the M3 integration -- the real stack run
# together over fixtures -- and, beside it, the place where two components that
# read one contract differently are caught.
loadforge_test_suite(loadforge_integration_m3_tests
  SOURCES full_signature_test.cpp states_test.cpp result_test.cpp real_kernel_test.cpp
          binary_test.cpp
  LINK loadforge_cli)
# The shipped binary, run for real (binary_test.cpp).
target_compile_definitions(loadforge_integration_m3_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_m3_tests loadforge)

# The disagreements: each test asserts what a contract says and fails against
# today's components. WILL_FAIL turns each into "expected to fail", so CI is
# green while the disagreement stands and red when the component is corrected,
# which is when the test is to move into the suite above (the journal fragment
# docs/journal/21-integration-m3.md has each, with its evidence).
#
# loadforge_test_suite() cannot say WILL_FAIL, so this is its body with the
# property added to the discovery. A test that fails by a sanitizer's report
# is "expected" too, so the same code runs in the passing suite above.
add_executable(loadforge_integration_m3_disagreements disagreements_test.cpp)
target_link_libraries(loadforge_integration_m3_disagreements PRIVATE
  loadforge_core loadforge_cli GTest::gtest_main GTest::gmock)
target_include_directories(loadforge_integration_m3_disagreements PRIVATE
  "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(loadforge_integration_m3_disagreements PRIVATE
  LOADFORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
loadforge_set_fp_contract(loadforge_integration_m3_disagreements)
loadforge_enable_sanitizers(loadforge_integration_m3_disagreements)
loadforge_enable_coverage(loadforge_integration_m3_disagreements)
target_compile_options(loadforge_integration_m3_disagreements PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
gtest_discover_tests(loadforge_integration_m3_disagreements PROPERTIES WILL_FAIL TRUE)
