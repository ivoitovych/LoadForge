# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/cache and nothing else.
#
# The cache calibration load. Links topology for the cache description it is
# sized from; the geometry is read through that, never assumed.
loadforge_component(loadforge_primitives_cache
  SOURCES
    cache.cpp
  LINK loadforge_topology)
