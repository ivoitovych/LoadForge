# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/capability and nothing else.
loadforge_component(loadforge_telemetry_capability
  SOURCES
    capability.cpp
  LINK loadforge_topology)
