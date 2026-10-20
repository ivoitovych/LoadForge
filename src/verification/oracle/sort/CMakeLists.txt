# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_component(loadforge_verification_oracle_sort SOURCES sort_oracle.cpp)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_sort PRIVATE -fprofile-update=atomic)
endif()
