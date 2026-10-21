# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/graph and nothing else.
#
# The independent oracle of the graph workload: the whole expected output --
# BFS distances and PageRank, in the arithmetic the specification pins --
# recomputed by scalar code and compared byte for byte.
loadforge_component(loadforge_verification_oracle_graph
  SOURCES graph_oracle.cpp)

# The oracle is called from several threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's plain counter increments then race and
# can come out negative. Atomic updates make the counts true.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_graph PRIVATE -fprofile-update=atomic)
endif()
