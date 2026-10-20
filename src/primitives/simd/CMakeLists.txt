# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): the top-level files of src/primitives/simd. The
# kernels under x86/ and arm/ belong to the streams that write them.
loadforge_component(loadforge_primitives_simd
  SOURCES
    select_path.cpp)
