# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T6 of src/primitives/simd/arm and nothing else.
#
# Built with the kernel library, so on AArch64 builds that are not
# LOADFORGE_SIMD=scalar, and nowhere else.
if(TARGET loadforge_primitives_simd_arm)
  loadforge_test_suite(loadforge_determinism_primitives_simd_arm_tests
    SOURCES gemm_neon_test.cpp
    LINK loadforge_primitives_simd_arm)
endif()
