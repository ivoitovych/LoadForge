# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/residual and nothing else.
#
# Derived tolerances: the bound a bounded check compares against, with its
# derivation written in residual.hpp. Depends on nothing but core.
loadforge_component(loadforge_verification_residual
  SOURCES
    residual.cpp)

# The oracle's check() runs on many threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's counters are plain increments: under
# coverage they race, lose counts, and gcovr reads negative branch counts and
# refuses the report. Atomic updates make the counters exact. This target
# only, because cmake/Coverage.cmake is not this module's to change (journal
# fragment 04 asks for it project-wide).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_residual PRIVATE -fprofile-update=atomic)
endif()
