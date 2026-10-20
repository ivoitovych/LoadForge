# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/integer and nothing else.
#
# The integer calibration load. Header-level dependencies (the Philox contract,
# the Executor contract, the determinism class) need no library; this one has
# an implementation file, hence a target.
loadforge_component(loadforge_primitives_integer
  SOURCES
    integer.cpp)
