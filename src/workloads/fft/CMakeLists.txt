# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/fft and nothing else.
#
# The FFT workload: the committed twiddle table, the radix-2 and radix-4
# Stockham stage kernels behind the dispatch seam, and the workload that
# generates a unit's input and computes its transform.
loadforge_component(loadforge_workloads_fft
  SOURCES
    kernel.cpp
    fft.cpp)

# The stage kernels run on the executor's threads; without atomic counter
# updates gcov counts can go negative under the coverage build.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_fft PRIVATE -fprofile-update=atomic)
endif()
