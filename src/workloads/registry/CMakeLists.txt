# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/registry and nothing else.
loadforge_component(loadforge_workloads_registry
  SOURCES
    registry.cpp
  LINK
    loadforge_workloads_adaptive_mesh
    loadforge_workloads_compression
    loadforge_workloads_dense_linear
    loadforge_workloads_fft
    loadforge_workloads_graph
    loadforge_workloads_monte_carlo
    loadforge_workloads_particles
    loadforge_workloads_raytrace
    loadforge_workloads_sort
    loadforge_workloads_sparse
    loadforge_workloads_stencil)
