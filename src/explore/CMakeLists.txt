# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/explore and nothing else.
#
# The explore mode's logic (F5): objectives bound to the capability report,
# successive halving over a curated candidate set, a reference candidate
# interleaved to measure and correct drift, and replicates with variance and
# confidence on every worst-case claim. It runs trials through the
# TrialRunner contract and owns no process, thread or system call.
loadforge_component(loadforge_explore
  SOURCES
    objective.cpp
    estimate.cpp
    drift.cpp
    search.cpp
  LINK loadforge_topology)
