# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/stencil and nothing else.
#
# The stencil workload: the sweep kernels behind the dispatch seam (kernels.cpp)
# and the workload that tiles a sweep over the executor (stencil.cpp).
loadforge_component(loadforge_workloads_stencil
  SOURCES
    kernels.cpp
    stencil.cpp)
