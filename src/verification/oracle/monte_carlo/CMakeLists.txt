# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_component(loadforge_verification_oracle_monte_carlo SOURCES monte_carlo_oracle.cpp)
if(LOADFORGE_COVERAGE)
  target_compile_options(loadforge_verification_oracle_monte_carlo PRIVATE -fprofile-update=atomic)
endif()
