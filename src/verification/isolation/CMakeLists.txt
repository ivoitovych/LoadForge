# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/isolation and nothing else.
#
# The provider of verification::Isolator (verification/isolation.hpp): the
# re-run ladder of docs/verification.md §7, and the choice of the core its
# second rung moves a unit to, made from the topology snapshot.
loadforge_component(loadforge_verification_isolation
  SOURCES
    ladder.cpp
    placement.cpp
  LINK loadforge_topology)
