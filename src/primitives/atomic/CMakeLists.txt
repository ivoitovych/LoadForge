# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/atomic and nothing else.
#
# The atomic calibration loads. They run on several threads, and under the
# coverage build that can make gcov's counters go negative, which gcovr then
# refuses to report (wave 1); -fprofile-update=atomic makes the counters
# themselves atomic.
loadforge_component(loadforge_primitives_atomic
  SOURCES
    atomic.cpp)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_primitives_atomic PRIVATE -fprofile-update=atomic)
endif()
