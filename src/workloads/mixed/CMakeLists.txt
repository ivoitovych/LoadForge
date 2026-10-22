# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/mixed and nothing else.
#
# The flagship mixed workload and thermal and power cycling, as schedules: data
# made before the run from its options alone, in the commands of
# worker/control.hpp. No process, no clock, no file: the controller carries a
# schedule out and the worker carries each command out, so this library links
# the core and nothing else (the control header it speaks is a contract header).
loadforge_component(loadforge_workloads_mixed
  SOURCES
    schedule.cpp
    roster.cpp
    mixed.cpp
    cycling.cpp)
