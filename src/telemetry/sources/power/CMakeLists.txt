# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): the package-power telemetry source and nothing else.
# Links the counters source for the one fact both share: what a failed
# perf_event_open means, and the remedy for it.
loadforge_component(loadforge_telemetry_sources_power
  SOURCES power.cpp
  LINK loadforge_platform loadforge_topology loadforge_telemetry_sources_counters)
