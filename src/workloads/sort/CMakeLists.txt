# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_component(loadforge_workloads_sort SOURCES sort.cpp)
# Worker threads under the coverage build must not make gcov's counters negative.
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_workloads_sort PRIVATE -fprofile-update=atomic)
endif()
