# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/integer and nothing else.
#
# The integer calibration load. Header-level dependencies (the Philox contract,
# the Executor contract, the determinism class) need no library; this one has
# an implementation file, hence a target.
loadforge_component(loadforge_primitives_integer
  SOURCES
    integer.cpp)

# The units run on the executor's threads, which under the coverage build can
# make gcov's counters go negative, and gcovr then refuses the report (wave 1).
# -fprofile-update=atomic makes the counters themselves atomic.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_primitives_integer PRIVATE -fprofile-update=atomic)
endif()
