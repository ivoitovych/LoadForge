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
