# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/platform/isa and nothing else.
#
# The ISA probe: CPUID and XCR0 on x86-64, the auxiliary vector's HWCAP on
# AArch64, read through a seam of three calls so every feature-bit combination
# is a unit test.
loadforge_component(loadforge_platform_isa
  SOURCES
    real_source.cpp
    detect.cpp
    probe.cpp)
