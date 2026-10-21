# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/platform/fpenv and nothing else.
#
# The floating-point environment: the register layouts, the real MXCSR / FPCR
# access, check(), and the establish and assert operations the worker and the
# controller call at startup.
loadforge_component(loadforge_platform_fpenv
  SOURCES
    check.cpp
    registers.cpp
    real_control.cpp
    environment.cpp)
