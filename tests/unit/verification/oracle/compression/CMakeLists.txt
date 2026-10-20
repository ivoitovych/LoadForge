# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T1 of src/verification/oracle/compression.
#
# The strict decoder rule by rule on blocks written from the format document,
# and the oracle against OracleContract (good units from the reference tool,
# wrong units for every rule) and its own refusals. lz4_support.hpp is shared
# with this module's T3 and T5 suites.
loadforge_test_suite(loadforge_unit_verification_oracle_compression_tests
  SOURCES
    decoder_test.cpp
    oracle_test.cpp
  LINK loadforge_verification_oracle_compression)
