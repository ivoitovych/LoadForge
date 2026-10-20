# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/raytrace and nothing else.
#
# The ray tracing workload's independent oracle: every primitive tried for each
# sampled pixel, written from workloads/raytrace/spec.hpp. The specification
# header is header-only and needs no link.
loadforge_component(loadforge_verification_oracle_raytrace SOURCES oracle.cpp)

# The contract's concurrency test runs check() from a thread per unit. gcov's
# counters are plain increments by default, so concurrent runs of one function
# lose updates and gcovr reads a negative count (journal fragment 05); atomic
# updates make the coverage of concurrently run code a number at all.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_raytrace PRIVATE -fprofile-update=atomic)
endif()
