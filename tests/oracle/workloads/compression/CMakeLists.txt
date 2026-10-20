# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 for src/workloads/compression and nothing else.
#
# The workload's output decodes, with a decoder written from the LZ4
# block-format document (lz4_decoder.hpp, also used by the property and
# fault-injection tiers), and that decoder is checked against blocks the real
# lz4 tool wrote (tests/fixtures/lz4-workload/, F21).
loadforge_test_suite(loadforge_oracle_workloads_compression_tests
  SOURCES
    round_trip_test.cpp
  LINK loadforge_workloads_compression)
