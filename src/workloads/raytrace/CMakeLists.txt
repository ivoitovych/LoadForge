# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/raytrace and nothing else.
#
# The ray tracing workload: a bounding volume hierarchy over a unit's
# primitives, traversed by packets of four rays, with a scalar kernel and one
# wide kernel per architecture (kernels.cpp). Header-only contracts above it
# (workload.hpp, executor.hpp, philox.hpp, dispatch.hpp, spec.hpp) need no
# target; loadforge_core carries the include path.
loadforge_component(loadforge_workloads_raytrace
  SOURCES
    bvh.cpp
    generator.cpp
    kernels.cpp
    raytrace.cpp)

# The wide kernel is written with the compilers' portable vector types and no
# intrinsic; a square root taken lane by lane must compile to the instruction,
# not to the instruction plus the errno path of libm (journal fragment 30).
# The vector types are passed only between functions that are always inlined,
# so the ABI note GCC prints for a 32-byte vector argument has nothing to say.
target_compile_options(loadforge_workloads_raytrace PRIVATE -fno-math-errno -Wno-psabi)

# The tests trace units on several threads at once; gcov's counters are plain
# increments by default and concurrent runs of one function lose updates
# (JOURNAL §1.22 area, fragment 05).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_raytrace PRIVATE -fprofile-update=atomic)
endif()
