# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/sources/frequency and nothing else.
loadforge_component(loadforge_telemetry_sources_frequency
  SOURCES
    frequency.cpp
  LINK loadforge_telemetry_sources_thermal loadforge_resources loadforge_topology)
