# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/resources and nothing else.
#
# What the kernel lets this process use and what it records when that runs
# out: MemAvailable, the memory cgroup's limit under either hierarchy, and the
# cgroup's OOM-kill count. Platform-level knowledge read through the seam, but
# classified with topology's capability model, so it sits above topology
# rather than inside platform, which nothing may make depend on topology.
loadforge_component(loadforge_resources
  SOURCES
    decimal.cpp
    cgroup.cpp
    memory_budget.cpp
    cgroup_oom_record.cpp
  LINK loadforge_topology loadforge_config)
