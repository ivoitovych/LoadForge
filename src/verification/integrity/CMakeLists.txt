# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/integrity and nothing else.
#
# The provider of verification/runner.hpp: the slice runner, the
# input-integrity bracket of verification.md §4a around every unit, the working
# set it runs them in, and the triplicated control state of PLAN F18 rule 3.
loadforge_component(loadforge_verification_integrity
  SOURCES
    layout.cpp
    difference.cpp
    locking.cpp
    control_state.cpp
    slice_runner.cpp
  LINK loadforge_platform)
