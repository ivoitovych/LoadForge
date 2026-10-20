# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/monte_carlo and nothing else.
#
# The Monte Carlo workload: Philox draws and the integrand's sum, one kernel
# per ISA path. -fno-math-errno lets the compiler turn the square root into
# the instruction alone (there is no errno path to branch to, and none to
# cover); it changes no result -- sqrt is correctly rounded either way.
loadforge_component(loadforge_workloads_monte_carlo
  SOURCES
    kernels.cpp
    monte_carlo.cpp)
target_compile_options(loadforge_workloads_monte_carlo PRIVATE -fno-math-errno)
