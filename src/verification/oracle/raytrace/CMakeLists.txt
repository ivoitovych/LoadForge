# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/raytrace and nothing else.
#
# The ray tracing workload's independent oracle: every primitive tried for each
# sampled pixel, written from workloads/raytrace/spec.hpp. The specification
# header is header-only and needs no link.
loadforge_component(loadforge_verification_oracle_raytrace SOURCES oracle.cpp)
