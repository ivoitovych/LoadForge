# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/invariant and nothing else.
#
# Invariant and conservation checks for later oracles: bounded ones judged by
# the residual module's derived tolerance, exact ones with no tolerance at all.
loadforge_component(loadforge_verification_invariant
  SOURCES
    invariant.cpp
  LINK loadforge_verification_residual)

# Every caller of these checks is an oracle's check(), which runs on many
# threads at once (OracleContract's ChecksRunConcurrently), and gcov's counters
# are plain increments: under coverage they race, and gcovr reads negative
# branch counts and refuses the report. Atomic updates make them exact, as for
# the residual and dense-linear oracle targets (journal fragment 04 asks for it
# project-wide in cmake/Coverage.cmake, which is not this module's to change).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_invariant PRIVATE -fprofile-update=atomic)
endif()
