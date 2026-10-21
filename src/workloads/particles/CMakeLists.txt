# SPDX-License-Identifier: GPL-3.0-or-later
#
# The particles workload (ADR 0004). The vector kernel is its own source file
# so that -mavx2 reaches nothing else: a standard inline function emitted in a
# unit built with AVX2 could be the copy the linker keeps for every caller.
loadforge_component(loadforge_workloads_particles
  SOURCES
    bin.cpp
    grid.cpp
    kernel_scalar.cpp
    kernel_vector.cpp
    kernels.cpp
    particles.cpp
  LINK loadforge_core)

if(NOT LOADFORGE_SIMD STREQUAL "scalar" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set_source_files_properties(kernel_vector.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()
