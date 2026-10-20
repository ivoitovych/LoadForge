# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/branch and nothing else.
#
# The branch calibration load: a data-dependent branch over generated values.
loadforge_component(loadforge_primitives_branch
  SOURCES
    branch.cpp)

# The units run on the executor's threads, which under the coverage build can
# make gcov's counters go negative, and gcovr then refuses the report (wave 1).
# -fprofile-update=atomic makes the counters themselves atomic.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_primitives_branch PRIVATE -fprofile-update=atomic)
endif()
