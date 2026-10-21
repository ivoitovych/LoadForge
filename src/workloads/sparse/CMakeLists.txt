# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/sparse and nothing else.
#
# The sparse (conjugate gradient) workload: generation of a unit, the scalar
# kernels behind the dispatch seam, and the fixed-decomposition solver that
# spreads a unit over the executor.
loadforge_component(loadforge_workloads_sparse
  SOURCES
    kernels.cpp
    sparse.cpp)
# compute() runs its kernels on the executor's threads, and gcov's counters
# are updated non-atomically by default: under threads they can go negative.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_sparse PRIVATE -fprofile-update=atomic)
endif()
