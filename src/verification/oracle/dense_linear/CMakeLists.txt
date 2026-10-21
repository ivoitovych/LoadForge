# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/dense_linear and nothing else.
#
# The independent oracle for the dense-linear workload, written from its
# specification and judged with the residual module's derived tolerance.
loadforge_component(loadforge_verification_oracle_dense_linear
  SOURCES
    dense_linear.cpp
  LINK loadforge_verification_residual)

# The oracle's check() runs on many threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's counters are plain increments: under
# coverage they race, lose counts, and gcovr reads negative branch counts and
# refuses the report. Atomic updates make the counters exact. This target
# only, because cmake/Coverage.cmake is not this module's to change (journal
# fragment 04 asks for it project-wide).
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_dense_linear PRIVATE -fprofile-update=atomic)
endif()
