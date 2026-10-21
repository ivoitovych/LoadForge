# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/adaptive_mesh and nothing else.
#
# The adaptive mesh workload's independent oracle: the mesh built by a plain
# recursion and the sampled coarse cells summed in leaf order, written from
# workloads/adaptive_mesh/spec.hpp. The specification header is header-only
# and needs no link.
loadforge_component(loadforge_verification_oracle_adaptive_mesh SOURCES oracle.cpp)

# The contract's concurrency test runs check() from a thread per unit. gcov's
# counters are plain increments by default, so concurrent runs of one function
# lose updates and gcovr reads a negative count (journal fragment 05); atomic
# updates make the coverage of concurrently run code a number at all.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_adaptive_mesh PRIVATE -fprofile-update=atomic)
endif()
