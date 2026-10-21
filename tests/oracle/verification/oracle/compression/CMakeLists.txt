# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T3 of src/verification/oracle/compression.
#
# The oracle against the reference tool: every block lz4 made, and every
# hand-built block lz4 decoded, in tests/fixtures/lz4/.
loadforge_test_suite(loadforge_oracle_verification_oracle_compression_tests
  SOURCES fixtures_test.cpp
  LINK loadforge_verification_oracle_compression)
