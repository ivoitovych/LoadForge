# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/stencil and nothing else.
#
# The independent oracle of the stencil workload: scalar sweeps of four sampled
# windows, compared with the output against a derived per-cell tolerance.
loadforge_component(loadforge_verification_oracle_stencil
  SOURCES stencil_oracle.cpp)

# The oracle is called from several threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's plain counter increments then race and
# can come out negative. Atomic updates make the counts true.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_stencil PRIVATE -fprofile-update=atomic)
endif()
