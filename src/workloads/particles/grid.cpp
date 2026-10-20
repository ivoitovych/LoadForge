// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/particles/grid.hpp"

#include <cstddef>
#include <cstdint>

#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles {

Grid make_grid(std::uint32_t cells_per_cutoff) noexcept {
  Grid grid;
  grid.cells_per_cutoff = cells_per_cutoff;
  grid.edge = 8 * cells_per_cutoff;
  grid.cells = grid.edge * grid.edge * grid.edge;
  grid.scale = static_cast<double>(cells_per_cutoff) / 2.0;
  return grid;
}

std::uint32_t cell_coordinate(double value, const Grid& grid) noexcept {
  if (!(value >= 0.0)) {
    return 0;
  }
  if (!(value < kBoxEdge)) {
    return grid.edge - 1;
  }
  return static_cast<std::uint32_t>(value * grid.scale);
}

namespace {

// The gap between two cells that are `offset` apart along one axis, in cells.
std::int32_t gap(std::int32_t offset) noexcept {
  const std::int32_t distance = offset < 0 ? -offset : offset;
  return distance > 1 ? distance - 1 : 0;
}

}  // namespace

RowTable make_row_table(std::uint32_t cells_per_cutoff) noexcept {
  const auto reach = static_cast<std::int32_t>(cells_per_cutoff);
  const std::int32_t limit = reach * reach;
  RowTable table;
  for (std::int32_t dz = -reach; dz <= reach; ++dz) {
    for (std::int32_t dy = -reach; dy <= reach; ++dy) {
      const std::int32_t used = gap(dy) * gap(dy) + gap(dz) * gap(dz);
      if (used >= limit) {
        continue;
      }
      std::int32_t dx_max = 0;
      for (std::int32_t dx = 1; dx <= reach; ++dx) {
        if (gap(dx) * gap(dx) < limit - used) {
          dx_max = dx;
        }
      }
      table.rows[table.count] = RowOffset{dy, dz, dx_max};
      ++table.count;
    }
  }
  return table;
}

Layout make_layout(const Grid& grid) noexcept {
  const auto aligned = [](std::size_t bytes) {
    return (bytes + kScratchAlignment - 1) / kScratchAlignment * kScratchAlignment;
  };
  const std::size_t index_bytes = kParticles * sizeof(std::uint32_t);
  const std::size_t coordinate_bytes = (kParticles + kMaxLanes) * sizeof(double);
  Layout layout;
  layout.cell_start = 0;
  layout.cell_of = layout.cell_start + aligned((grid.cells + 1U) * sizeof(std::uint32_t));
  layout.order = layout.cell_of + aligned(index_bytes);
  layout.x = layout.order + aligned(index_bytes);
  layout.y = layout.x + aligned(coordinate_bytes);
  layout.z = layout.y + aligned(coordinate_bytes);
  layout.force = layout.z + aligned(coordinate_bytes);
  layout.total = layout.force + aligned(3 * kParticles * sizeof(double));
  return layout;
}

}  // namespace loadforge::workloads::particles
