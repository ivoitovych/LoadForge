# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/golden and nothing else.
#
# Golden vectors: the file, recording one from a workload, and comparing a
# recorded vector with a fresh one into a regression report -- a tripwire,
# never an oracle and never a hardware verdict (verification.md §4).
loadforge_component(loadforge_verification_golden
  SOURCES
    golden.cpp
  LINK loadforge_verification_checksum)
