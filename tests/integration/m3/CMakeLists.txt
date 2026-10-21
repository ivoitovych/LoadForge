# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of the M3 integration -- the real stack run
# together over fixtures. corrections_test.cpp is the seven places where two
# components read one contract differently, now corrected and passing.
loadforge_test_suite(loadforge_integration_m3_tests
  SOURCES full_signature_test.cpp states_test.cpp result_test.cpp real_kernel_test.cpp
          binary_test.cpp containment_test.cpp corrections_test.cpp
  LINK loadforge_cli)
# The shipped binary, run for real (binary_test.cpp).
target_compile_definitions(loadforge_integration_m3_tests PRIVATE
  LOADFORGE_BINARY="$<TARGET_FILE:loadforge>")
add_dependencies(loadforge_integration_m3_tests loadforge)
