# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/graph and nothing else.
#
# The graph workload: a parallel direction-optimising BFS and a parallel
# pull-based PageRank, with the PageRank sweep behind the dispatch seam. It
# needs only headers of other components (the executor, the generator, the
# dispatch seam), so it links nothing but core -- and the vector sweeps, which
# are libraries of their own under src/primitives/simd/ (the portability
# workflow refuses intrinsics anywhere else), built only on their architecture
# and never in a LOADFORGE_SIMD=scalar build. Each defines the macro that puts
# its sweep in this workload's table, so the table holds a kernel exactly when
# one was built. The fragments are added in sorted order, src/primitives before
# src/workloads, so the targets exist by now.
set(graph_kernels "")
foreach(kernels IN ITEMS loadforge_primitives_simd_x86 loadforge_primitives_simd_arm)
  if(TARGET ${kernels})
    list(APPEND graph_kernels ${kernels})
  endif()
endforeach()

loadforge_component(loadforge_workloads_graph
  SOURCES
    kernels.cpp
    graph.cpp
  LINK ${graph_kernels})
