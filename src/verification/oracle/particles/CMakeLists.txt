# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/particles and nothing else.
#
# The independent oracle of the particles workload: the direct sum of the
# short-range force on sampled particles over all the others, compared with the
# output against a tolerance derived from the neighbour count and the terms.
loadforge_component(loadforge_verification_oracle_particles
  SOURCES particles_oracle.cpp)

# The oracle is called from several threads at once (OracleContract's
# ChecksRunConcurrently), and gcov's plain counter increments then race and
# can come out negative. Atomic updates make the counts true.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_particles PRIVATE -fprofile-update=atomic)
endif()
