# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/sparse and nothing else.
#
# The independent oracle of the sparse workload: the explicit residual of the
# output's iterate by a scalar SpMV, row by row, and the residual norm against
# the conjugate-gradient bound.
loadforge_component(loadforge_verification_oracle_sparse
  SOURCES sparse_oracle.cpp)

# The oracle is called from several threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's plain counter increments then race and
# can come out negative. Atomic updates make the counts true.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_sparse PRIVATE -fprofile-update=atomic)
endif()
