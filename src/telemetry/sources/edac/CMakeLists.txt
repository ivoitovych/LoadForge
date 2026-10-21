# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): the EDAC telemetry source and nothing else.
loadforge_component(loadforge_telemetry_sources_edac
  SOURCES edac.cpp
  LINK loadforge_platform loadforge_topology)
