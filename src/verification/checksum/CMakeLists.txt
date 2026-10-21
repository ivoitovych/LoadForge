# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/checksum and nothing else.
#
# Exact comparison into a Difference, and the CRC-64 seal that guards input
# which cannot be regenerated (verification.md §4a rule 2).
loadforge_component(loadforge_verification_checksum
  SOURCES
    compare.cpp
    crc64.cpp
    seal.cpp)
