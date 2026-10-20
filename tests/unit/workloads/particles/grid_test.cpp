// SPDX-License-Identifier: GPL-3.0-or-later
//
// The geometry of the neighbour search: grid, cell of a coordinate, rows of
// cells that can interact, scratch layout.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "workloads/particles/grid.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles {
namespace {

TEST(ParticlesGrid, TheGridIsEightCellsPerCutoffEdgeCubed) {
  const Grid one = make_grid(1);
  EXPECT_EQ(one.cells_per_cutoff, 1U);
  EXPECT_EQ(one.edge, 8U);
  EXPECT_EQ(one.cells, 512U);
  EXPECT_EQ(one.scale, 0.5);
  const Grid two = make_grid(2);
  EXPECT_EQ(two.edge, 16U);
  EXPECT_EQ(two.cells, 4096U);
  EXPECT_EQ(two.scale, 1.0);
  const Grid four = make_grid(4);
  EXPECT_EQ(four.edge, 32U);
  EXPECT_EQ(four.cells, 32768U);
  EXPECT_EQ(four.scale, 2.0);
}

TEST(ParticlesGrid, ACellIsTheCutoffOverCellsPerCutoffWide) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    const double edge = 2.0 / static_cast<double>(c);
    EXPECT_EQ(static_cast<double>(grid.edge) * edge, kBoxEdge) << c;
    EXPECT_EQ(kCutoffSquared, 4.0);
  }
}

TEST(ParticlesGrid, CellCoordinatesSplitAtExactMultiplesOfTheCellEdge) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    const double edge = 2.0 / static_cast<double>(c);
    for (std::uint32_t cell = 0; cell < grid.edge; ++cell) {
      const double start = static_cast<double>(cell) * edge;
      EXPECT_EQ(cell_coordinate(start, grid), cell) << "c " << c << " start of cell " << cell;
      if (cell > 0) {
        EXPECT_EQ(cell_coordinate(std::nextafter(start, 0.0), grid), cell - 1)
            << "c " << c << " just below cell " << cell;
      }
      EXPECT_EQ(cell_coordinate(start + 0.5 * edge, grid), cell);
    }
  }
}

TEST(ParticlesGrid, TheCoordinateJustBelowTheBoxEdgeIsInTheLastCell) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    EXPECT_EQ(cell_coordinate(kBoxEdge - std::ldexp(1.0, -49), grid), grid.edge - 1);
    EXPECT_EQ(cell_coordinate(std::nextafter(kBoxEdge, 0.0), grid), grid.edge - 1);
  }
}

TEST(ParticlesGrid, ACoordinateOutsideTheBoxOrNotANumberLandsInTheNearestCell) {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    EXPECT_EQ(cell_coordinate(kBoxEdge, grid), grid.edge - 1) << "the edge itself";
    EXPECT_EQ(cell_coordinate(1e300, grid), grid.edge - 1);
    EXPECT_EQ(cell_coordinate(inf, grid), grid.edge - 1);
    EXPECT_EQ(cell_coordinate(-inf, grid), 0U);
    EXPECT_EQ(cell_coordinate(-1.0, grid), 0U);
    EXPECT_EQ(cell_coordinate(-std::numeric_limits<double>::denorm_min(), grid), 0U);
    EXPECT_EQ(cell_coordinate(-0.0, grid), 0U);
    EXPECT_EQ(cell_coordinate(nan, grid), 0U);
    EXPECT_EQ(cell_coordinate(-nan, grid), 0U);
  }
}

// How far apart, in cells, two cells are along an axis, as the gap between
// them: the cells (a, a + d) are separated by max(|d| - 1, 0) whole cells.
// The nearest points of two cells are that many edges apart on each axis, so
// no pair of particles in them is closer than the root of the sum of squares.
bool within_reach(int dx, int dy, int dz, int c) {
  const auto gap = [](int d) { return std::max(std::abs(d) - 1, 0); };
  return gap(dx) * gap(dx) + gap(dy) * gap(dy) + gap(dz) * gap(dz) < c * c;
}

TEST(ParticlesGrid, TheRowsCoverExactlyTheCellsThatCanHoldANeighbour) {
  for (const std::uint32_t cells : kCellsPerCutoff) {
    const int c = static_cast<int>(cells);
    const RowTable table = make_row_table(cells);
    for (int dz = -c; dz <= c; ++dz) {
      for (int dy = -c; dy <= c; ++dy) {
        for (int dx = -c; dx <= c; ++dx) {
          int covering = 0;
          for (std::size_t r = 0; r < table.count; ++r) {
            const RowOffset& row = table.rows[r];
            if (row.dy == dy && row.dz == dz && std::abs(dx) <= row.dx_max) {
              ++covering;
            }
          }
          EXPECT_EQ(covering, within_reach(dx, dy, dz, c) ? 1 : 0)
              << "c " << c << " offset " << dx << " " << dy << " " << dz;
        }
      }
    }
  }
}

TEST(ParticlesGrid, EveryPairWithinTheCutoffIsInCellsTheRowsVisit) {
  // The claim the pruning rests on, checked on points rather than on cell
  // geometry: a particle pair closer than the cutoff is in cells that
  // within_reach admits. Pairs on a fine lattice, around cell boundaries.
  for (const std::uint32_t cells : kCellsPerCutoff) {
    const int c = static_cast<int>(cells);
    const Grid grid = make_grid(cells);
    const double step = 2.0 / static_cast<double>(c) / 4.0;  // a quarter of a cell
    for (int ax = 0; ax < 12; ++ax) {
      for (int bx = 0; bx < 40; ++bx) {
        for (int by = -4; by <= 8; ++by) {
          const double xa = 4.0 + ax * step;
          const double xb = 4.0 + bx * step;
          const double yb = 4.0 + by * step;
          const double q = (xa - xb) * (xa - xb) + (4.0 - yb) * (4.0 - yb);
          if (q >= kCutoffSquared) {
            continue;
          }
          const int dx = static_cast<int>(cell_coordinate(xb, grid)) -
                         static_cast<int>(cell_coordinate(xa, grid));
          const int dy = static_cast<int>(cell_coordinate(yb, grid)) -
                         static_cast<int>(cell_coordinate(4.0, grid));
          EXPECT_TRUE(within_reach(dx, dy, 0, c)) << "c " << c << " " << xa << " " << xb;
        }
      }
    }
  }
}

TEST(ParticlesGrid, TheRowTablesAreWhatTheGeometryGives) {
  const RowTable one = make_row_table(1);
  EXPECT_EQ(one.count, 9U) << "27 cells, nothing pruned";
  for (std::size_t r = 0; r < one.count; ++r) {
    EXPECT_EQ(one.rows[r].dx_max, 1);
  }
  const RowTable two = make_row_table(2);
  EXPECT_EQ(two.count, 25U) << "125 cells: a gap of one cell is still inside the cutoff";
  for (std::size_t r = 0; r < two.count; ++r) {
    EXPECT_EQ(two.rows[r].dx_max, 2);
  }
  const RowTable four = make_row_table(4);
  EXPECT_EQ(four.count, 77U) << "the four rows (+-4, +-4) hold no cell in reach";
  std::size_t visited = 0;
  for (std::size_t r = 0; r < four.count; ++r) {
    visited += 2U * static_cast<std::size_t>(four.rows[r].dx_max) + 1U;
  }
  EXPECT_EQ(visited, 613U) << "of 729";
}

TEST(ParticlesGrid, RowsAreVisitedByIncreasingZThenY) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const RowTable table = make_row_table(c);
    for (std::size_t r = 1; r < table.count; ++r) {
      const RowOffset& before = table.rows[r - 1];
      const RowOffset& after = table.rows[r];
      EXPECT_TRUE(before.dz < after.dz || (before.dz == after.dz && before.dy < after.dy));
    }
    EXPECT_LE(table.count, kMaxRows);
  }
  const RowTable four = make_row_table(4);
  EXPECT_EQ(four.rows[0].dz, -4);
  EXPECT_EQ(four.rows[0].dy, -3);
  EXPECT_EQ(four.rows[0].dx_max, 2) << "gaps 3 and 2 use 13 of 16: only the gaps up to 2 fit";
}

TEST(ParticlesGrid, TheLayoutHoldsEachArrayAlignedAndApart) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    const Layout layout = make_layout(grid);
    struct Region {
      std::size_t offset;
      std::size_t bytes;
    };
    const std::size_t index_bytes = kParticles * 4;
    const std::size_t coordinate_bytes = (kParticles + kMaxLanes) * 8;
    const Region regions[] = {
        {layout.cell_start, (grid.cells + 1U) * 4},
        {layout.cell_of, index_bytes},
        {layout.order, index_bytes},
        {layout.x, coordinate_bytes},
        {layout.y, coordinate_bytes},
        {layout.z, coordinate_bytes},
        {layout.force, 3 * kParticles * 8},
    };
    std::size_t end = 0;
    for (const Region& region : regions) {
      EXPECT_EQ(region.offset % kScratchAlignment, 0U);
      EXPECT_GE(region.offset, end) << "regions do not overlap, in order";
      end = region.offset + region.bytes;
    }
    EXPECT_GE(layout.total, end);
    EXPECT_EQ(layout.total % kScratchAlignment, 0U);
    EXPECT_LT(layout.total - end, kScratchAlignment) << "no more slack than the alignment";
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
