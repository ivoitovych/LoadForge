# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/cache and nothing else.
#
# The cache calibration load. Links topology for the cache description it is
# sized from; the geometry is read through that, never assumed.
loadforge_component(loadforge_primitives_cache
  SOURCES
    cache.cpp
  LINK loadforge_topology)

# The units run on the executor's threads, which under the coverage build can
# make gcov's counters go negative, and gcovr then refuses the report (wave 1).
# -fprofile-update=atomic makes the counters themselves atomic.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_primitives_cache PRIVATE -fprofile-update=atomic)
endif()
