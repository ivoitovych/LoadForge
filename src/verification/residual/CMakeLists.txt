# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/residual and nothing else.
#
# Derived tolerances: the bound a bounded check compares against, with its
# derivation written in residual.hpp. Depends on nothing but core.
loadforge_component(loadforge_verification_residual
  SOURCES
    residual.cpp)
