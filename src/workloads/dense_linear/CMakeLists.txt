# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/workloads/dense_linear and nothing else.
#
# The dense-linear workload and its scalar kernel. The vector kernels are
# libraries of their own under src/primitives/simd/, built only on their
# architecture and never in a LOADFORGE_SIMD=scalar build; each defines the
# macro that puts its kernel in this workload's table, so the table holds a
# kernel exactly when one was built. The fragments are added in sorted order,
# src/primitives before src/workloads, so the targets exist by now.
set(dense_linear_kernels "")
foreach(kernels IN ITEMS loadforge_primitives_simd_x86 loadforge_primitives_simd_arm)
  if(TARGET ${kernels})
    list(APPEND dense_linear_kernels ${kernels})
  endif()
endforeach()

loadforge_component(loadforge_workloads_dense_linear
  SOURCES
    dense_linear.cpp
    scalar_kernel.cpp
  LINK ${dense_linear_kernels})

# Coverage counters updated atomically: the determinism tier runs this code on
# up to seven threads at once, and gcov's plain increments then lose updates --
# enough that gcov derived a negative fall-through count for a kernel loop and
# gcovr refused the report (docs/journal/06-workload-dense-linear.md).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_dense_linear PRIVATE -fprofile-update=atomic)
endif()
