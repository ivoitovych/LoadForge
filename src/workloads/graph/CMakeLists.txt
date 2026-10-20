# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/graph and nothing else.
#
# The graph workload: a parallel direction-optimising BFS and a parallel
# pull-based PageRank, with the PageRank sweep behind the dispatch seam. It
# needs only headers of other components (the executor, the generator, the
# dispatch seam), so it links nothing but core.
loadforge_component(loadforge_workloads_graph
  SOURCES
    kernels.cpp
    graph.cpp)
