# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/floating and nothing else.
#
# The floating-point calibration load. Built under the project-wide
# -ffp-contract=off (loadforge_component applies it), so the only fusion in it
# is the std::fma that is written.
loadforge_component(loadforge_primitives_floating
  SOURCES
    floating.cpp)
