# SPDX-License-Identifier: GPL-3.0-or-later
loadforge_component(loadforge_verification_oracle_registry
  SOURCES
    registry.cpp
  LINK
    loadforge_verification_oracle_compression
    loadforge_verification_oracle_dense_linear
    loadforge_verification_oracle_monte_carlo
    loadforge_verification_oracle_sort)
