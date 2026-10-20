# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/primitives/memory and nothing else.
#
# The memory calibration load: streaming copy and read-back over the caller's
# buffers. No allocation happens here, so no allocation failure to inject.
loadforge_component(loadforge_primitives_memory
  SOURCES
    memory.cpp)
