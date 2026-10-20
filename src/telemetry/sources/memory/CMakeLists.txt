# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/sources/memory and nothing else.
loadforge_component(loadforge_telemetry_sources_memory
  SOURCES
    memory.cpp
  LINK loadforge_telemetry_sources_thermal loadforge_resources loadforge_topology)
