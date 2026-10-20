# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/telemetry/sources/thermal and nothing else.
#
# Temperatures from hwmon and the thermal zones, and the pieces the other
# sysfs-reading sources of this stream share: Channel (one integer attribute
# read as a gauge, with the remedy for each way it can fail) and the hwmon
# chip discovery. fan and frequency link this library for them.
loadforge_component(loadforge_telemetry_sources_thermal
  SOURCES
    channel.cpp
    hwmon.cpp
    thermal.cpp
  LINK loadforge_topology loadforge_resources loadforge_platform)
