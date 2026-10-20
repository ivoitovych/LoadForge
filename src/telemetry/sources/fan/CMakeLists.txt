# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/sources/fan and nothing else.
loadforge_component(loadforge_telemetry_sources_fan
  SOURCES
    fan.cpp
  LINK loadforge_telemetry_sources_thermal loadforge_topology)
