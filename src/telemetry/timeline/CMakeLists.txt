# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/timeline and nothing else.
loadforge_component(loadforge_telemetry_timeline
  SOURCES
    bounded_timeline.cpp)
