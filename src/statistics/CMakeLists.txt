# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/statistics and nothing else.
#
# The Load Signature of a window: a pure, compensated, order-fixed summary of
# a timeline (signature.hpp is the contract).
loadforge_component(loadforge_statistics
  SOURCES
    summarizer.cpp
  LINK loadforge_topology)
