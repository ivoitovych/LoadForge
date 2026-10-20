# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 for src/workloads/compression and nothing else.
#
# The workload against WorkloadContract (which it provides), its own
# executors against ExecutorContract (which it consumes), the factory and the
# output layout, the compressor's encoding at every boundary, and the
# generator.
loadforge_test_suite(loadforge_unit_workloads_compression_tests
  SOURCES
    compression_test.cpp
    executors_test.cpp
    generator_test.cpp
    lz4_block_test.cpp
  LINK loadforge_workloads_compression)
