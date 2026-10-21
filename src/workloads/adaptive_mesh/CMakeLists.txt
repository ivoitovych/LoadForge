# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/adaptive_mesh and nothing else.
#
# The adaptive mesh workload. Its leaves are claimed by the executor's threads
# and their values added into per-slot partial sums, so the determinism and
# fault-injection tiers run this code on several threads at once; gcov's plain
# counter increments then lose updates and a negative count can stop the
# report (docs/journal/06-workload-dense-linear.md), hence atomic updates.
loadforge_component(loadforge_workloads_adaptive_mesh
  SOURCES
    adaptive_mesh.cpp)

if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_adaptive_mesh PRIVATE -fprofile-update=atomic)
endif()
