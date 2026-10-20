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
