# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/simd/x86 and nothing else.
#
# The x86-64 vector kernels: the only directory in which x86 intrinsics may
# appear (the portability workflow refuses them anywhere else). Built on
# x86-64 unless the build is LOADFORGE_SIMD=scalar, which exists to prove the
# portable code needs none of them. No -mavx2 here: each kernel carries a
# target attribute, so only its own code is AVX2 (gemm_avx2.hpp says why).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT LOADFORGE_SIMD STREQUAL "scalar")
  loadforge_component(loadforge_primitives_simd_x86 SOURCES gemm_avx2.cpp)
  target_compile_definitions(loadforge_primitives_simd_x86 PUBLIC LOADFORGE_SIMD_X86_KERNELS=1)
  # Atomic coverage counters: the kernel runs on several threads at once
  # (src/workloads/dense_linear/CMakeLists.txt says what plain ones did).
  if(LOADFORGE_COVERAGE)
    target_compile_options(loadforge_primitives_simd_x86 PRIVATE -fprofile-update=atomic)
  endif()
endif()
