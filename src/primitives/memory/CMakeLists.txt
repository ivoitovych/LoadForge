# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/memory and nothing else.
#
# The memory calibration load: streaming copy and read-back over the caller's
# buffers. No allocation happens here, so no allocation failure to inject.
loadforge_component(loadforge_primitives_memory
  SOURCES
    memory.cpp)

# The units run on the executor's threads, which under the coverage build can
# make gcov's counters go negative, and gcovr then refuses the report (wave 1).
# -fprofile-update=atomic makes the counters themselves atomic.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_primitives_memory PRIVATE -fprofile-update=atomic)
endif()
