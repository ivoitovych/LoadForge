# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/branch and nothing else.
#
# The branch calibration load: a data-dependent branch over generated values.
loadforge_component(loadforge_primitives_branch
  SOURCES
    branch.cpp)
