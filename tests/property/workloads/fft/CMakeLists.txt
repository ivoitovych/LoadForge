# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the property tests (T4) of src/workloads/fft and nothing else.
loadforge_test_suite(loadforge_property_workloads_fft_tests
  SOURCES property_test.cpp
  LINK loadforge_workloads_fft)

# Real threads under the coverage build (see src/workloads/fft/CMakeLists.txt).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_property_workloads_fft_tests PRIVATE -fprofile-update=atomic)
endif()
