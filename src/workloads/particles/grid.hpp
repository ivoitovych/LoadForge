// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_GRID_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_GRID_HPP

// The geometry of the neighbour search: the cells, which cells a cell must
// look at, and where everything lives in the scratch (spec.hpp, DECOMPOSITION).

#include <array>
#include <cstddef>
#include <cstdint>

#include "workloads/particles/kernel.hpp"

namespace loadforge::workloads::particles {

/// The box divided into cubes of edge rc / c, with c the cells per cutoff.
///
///   edge   cells along one side: kBoxEdge / (rc / c) = 8 c
///   cells  edge^3
///   scale  c / 2, a power of two, so value * scale is exact and its integer
///          part is the exact cell of the coordinate
struct Grid {
  std::uint32_t cells_per_cutoff = 1;
  std::uint32_t edge = 0;
  std::uint32_t cells = 0;
  double scale = 0.0;
};

/// Precondition: is_cells_per_cutoff(cells_per_cutoff).
[[nodiscard]] Grid make_grid(std::uint32_t cells_per_cutoff) noexcept;

/// The cell, along one axis, of a coordinate. Total: a coordinate that is
/// NaN, negative, or not below the box edge (a corrupted input; the generator
/// makes none) lands in the nearest cell, so every particle is in a cell and
/// the search does a bounded amount of work whatever it was handed.
[[nodiscard]] std::uint32_t cell_coordinate(double value, const Grid& grid) noexcept;

/// One row of cells: the cells (x, y + dy, z + dz) with |x - cell x| <= dx_max
/// are the ones in that row that can hold a particle within the cutoff of a
/// particle of the cell (x, y, z).
struct RowOffset {
  std::int32_t dy = 0;
  std::int32_t dz = 0;
  std::int32_t dx_max = 0;
};

/// A cell at offset (dx, dy, dz) can hold a particle within the cutoff only if
/// the gap between the two cells, (max(|dx| - 1, 0), ...) cell edges per axis,
/// is shorter than rc: the sum of squares of the three gaps is below c^2.
/// Rows with no such cell are absent; within a row the cells are contiguous in
/// memory order, so a row is one run of sorted particles.
inline constexpr std::size_t kMaxRows = 81;  // (2 * 4 + 1)^2, for c = 4
struct RowTable {
  std::array<RowOffset, kMaxRows> rows{};
  std::size_t count = 0;
};

/// Rows in the order the terms are visited: dz, then dy, both increasing.
/// Precondition: is_cells_per_cutoff(cells_per_cutoff).
[[nodiscard]] RowTable make_row_table(std::uint32_t cells_per_cutoff) noexcept;

/// Byte offsets, from the aligned start of the scratch, of what the search
/// keeps there, each 64-byte aligned, and the total.
///
///   cell_start  cells + 1 u32: the first sorted particle of each cell
///   cell_of     kParticles u32: a particle's cell, between the two passes
///   order       kParticles u32: the particle at each sorted position
///   x, y, z     kParticles + kMaxLanes doubles: the sorted coordinates
///   force       3 kParticles doubles: the force at each sorted position
struct Layout {
  std::size_t cell_start = 0;
  std::size_t cell_of = 0;
  std::size_t order = 0;
  std::size_t x = 0;
  std::size_t y = 0;
  std::size_t z = 0;
  std::size_t force = 0;
  std::size_t total = 0;
};

inline constexpr std::size_t kScratchAlignment = 64;

[[nodiscard]] Layout make_layout(const Grid& grid) noexcept;

}  // namespace loadforge::workloads::particles

#endif
