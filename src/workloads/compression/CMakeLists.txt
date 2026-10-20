# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/compression and nothing else.
#
# The compression workload (PLAN F14): an LZ4-block compressor writing the
# output spec.hpp defines, and its input generator over Philox. Header-only
# contracts above it (workload.hpp, executor.hpp, philox.hpp, dispatch.hpp)
# need no target; loadforge_core carries the include path.
loadforge_component(loadforge_workloads_compression
  SOURCES
    lz4_block.cpp
    generator.cpp
    compression.cpp)

# The tests compress units on several threads at once (the T6 thread counts,
# and the workload contract's concurrent units). gcov's counters are plain
# increments by default, so concurrent runs of one function lose updates and
# gcovr then reads a negative count and refuses the report (journal fragment
# 05, measured). Atomic updates are what makes the coverage of concurrently
# run code a number at all.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_compression PRIVATE -fprofile-update=atomic)
endif()
