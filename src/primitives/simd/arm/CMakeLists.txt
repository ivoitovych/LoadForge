# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/simd/arm and nothing else.
#
# The AArch64 vector kernels. Built on AArch64 unless the build is
# LOADFORGE_SIMD=scalar; never compiled on x86-64, which is why the coverage
# job also runs on ARM64 and the completeness check expects this directory
# only there (tools/check-coverage-complete.sh).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND NOT LOADFORGE_SIMD STREQUAL "scalar")
  loadforge_component(loadforge_primitives_simd_arm SOURCES gemm_neon.cpp)
  target_compile_definitions(loadforge_primitives_simd_arm PUBLIC LOADFORGE_SIMD_ARM_KERNELS=1)
  # Atomic coverage counters: the kernel runs on several threads at once
  # (src/workloads/dense_linear/CMakeLists.txt says what plain ones did).
  if(LOADFORGE_COVERAGE)
    target_compile_options(loadforge_primitives_simd_arm PRIVATE -fprofile-update=atomic)
  endif()
endif()
