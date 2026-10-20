# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/primitives/simd/x86 and nothing else.
#
# Built with the kernel library, so on x86-64 builds that are not
# LOADFORGE_SIMD=scalar, and nowhere else.
if(TARGET loadforge_primitives_simd_x86)
  loadforge_test_suite(loadforge_determinism_primitives_simd_x86_tests
    SOURCES gemm_avx2_test.cpp
    LINK loadforge_primitives_simd_x86)
endif()
