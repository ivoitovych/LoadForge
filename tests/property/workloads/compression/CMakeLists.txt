# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T4 for src/workloads/compression and nothing else.
#
# The block within compress_bound() on the inputs closest to it, the
# generator's compressibility inside the fifth to four fifths spec.hpp asks
# for, and the round trip through the test decoder at every length boundary.
loadforge_test_suite(loadforge_property_workloads_compression_tests
  SOURCES
    properties_test.cpp
  LINK loadforge_workloads_compression)
