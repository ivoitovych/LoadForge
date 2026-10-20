// SPDX-License-Identifier: GPL-3.0-or-later
//
// The neighbour search: carving the scratch, sorting into cells, listing the
// runs of a cell, putting the forces back in the input's order.

#include "workloads/particles/bin.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/particles/support.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

constexpr std::size_t kSlack = kScratchAlignment;

struct Scratch {
  explicit Scratch(const Grid& grid, std::byte fill = std::byte{0}, std::size_t skew = 0)
      : layout(make_layout(grid)), storage(layout.total + kSlack + 64, fill), skew(skew) {}
  std::span<std::byte> span() {
    return std::span<std::byte>(storage).subspan(skew, layout.total + kSlack);
  }
  Layout layout;
  Bytes storage;
  std::size_t skew;
};

Bytes generated_input(std::uint64_t seed, std::uint64_t index) {
  std::vector<double> positions(3 * kParticles);
  for (std::size_t element = 0; element < positions.size(); ++element) {
    positions[element] = position_value(rng::u64_of(rng::block_at(seed, index, element)));
  }
  return testing::as_bytes(positions);
}

TEST(ParticlesBin, CarvingGivesAlignedDisjointArraysInsideTheScratchWhateverItsAddress) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    for (std::size_t skew = 0; skew < 64; skew += 7) {
      Scratch scratch(grid, std::byte{0}, skew);
      const std::span<std::byte> bytes = scratch.span();
      const Binned binned = carve(bytes, scratch.layout, grid);
      EXPECT_EQ(binned.cell_start.size(), grid.cells + 1U);
      EXPECT_EQ(binned.cell_of.size(), kParticles);
      EXPECT_EQ(binned.order.size(), kParticles);
      EXPECT_EQ(binned.x.size(), kParticles + kMaxLanes);
      EXPECT_EQ(binned.y.size(), kParticles + kMaxLanes);
      EXPECT_EQ(binned.z.size(), kParticles + kMaxLanes);
      EXPECT_EQ(binned.force.size(), 3 * kParticles);
      struct Range {
        std::uintptr_t begin;
        std::uintptr_t end;
      };
      const auto range = [](const auto& span) {
        const auto begin = reinterpret_cast<std::uintptr_t>(span.data());
        return Range{begin, begin + span.size_bytes()};
      };
      std::vector<Range> ranges = {
          range(binned.cell_start), range(binned.cell_of), range(binned.order), range(binned.x),
          range(binned.y),          range(binned.z),       range(binned.force)};
      std::ranges::sort(ranges, {}, &Range::begin);
      const auto low = reinterpret_cast<std::uintptr_t>(bytes.data());
      const auto high = low + bytes.size();
      for (std::size_t i = 0; i < ranges.size(); ++i) {
        EXPECT_EQ(ranges[i].begin % kScratchAlignment, 0U);
        EXPECT_GE(ranges[i].begin, low);
        EXPECT_LE(ranges[i].end, high);
        if (i > 0) {
          EXPECT_LE(ranges[i - 1].end, ranges[i].begin);
        }
      }
    }
  }
}

TEST(ParticlesBin, SortingFillsEveryEntryTheSearchReadsWhateverTheScratchHeld) {
  const Bytes input = generated_input(3, 1);
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    Scratch zeros(grid, std::byte{0x00});
    Scratch ones(grid, std::byte{0xFF}, 5);
    const Binned a = carve(zeros.span(), zeros.layout, grid);
    const Binned b = carve(ones.span(), ones.layout, grid);
    sort_into_cells(input, grid, a);
    sort_into_cells(input, grid, b);
    EXPECT_TRUE(std::ranges::equal(a.cell_start, b.cell_start));
    EXPECT_TRUE(std::ranges::equal(a.order, b.order));
    EXPECT_TRUE(std::ranges::equal(a.x, b.x));
    EXPECT_TRUE(std::ranges::equal(a.y, b.y));
    EXPECT_TRUE(std::ranges::equal(a.z, b.z));
  }
}

TEST(ParticlesBin, ParticlesAreSortedIntoTheirCellsKeepingInputOrderWithinACell) {
  const Bytes input = generated_input(11, 4);
  const std::vector<double> positions = testing::as_doubles(input);
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    Scratch scratch(grid);
    const Binned binned = carve(scratch.span(), scratch.layout, grid);
    sort_into_cells(input, grid, binned);
    EXPECT_EQ(binned.cell_start[0], 0U);
    EXPECT_EQ(binned.cell_start[grid.cells], kParticles);
    EXPECT_TRUE(std::ranges::is_sorted(binned.cell_start));
    std::vector<bool> seen(kParticles, false);
    for (std::uint32_t cell = 0; cell < grid.cells; ++cell) {
      const std::uint32_t iz = cell / (grid.edge * grid.edge);
      const std::uint32_t iy = cell / grid.edge % grid.edge;
      const std::uint32_t ix = cell % grid.edge;
      for (std::uint32_t at = binned.cell_start[cell]; at < binned.cell_start[cell + 1]; ++at) {
        const std::size_t particle = binned.order[at];
        ASSERT_LT(particle, kParticles);
        EXPECT_FALSE(seen[particle]) << "each particle once";
        seen[particle] = true;
        EXPECT_EQ(binned.x[at], positions[3 * particle]);
        EXPECT_EQ(binned.y[at], positions[3 * particle + 1]);
        EXPECT_EQ(binned.z[at], positions[3 * particle + 2]);
        EXPECT_EQ(cell_coordinate(binned.x[at], grid), ix);
        EXPECT_EQ(cell_coordinate(binned.y[at], grid), iy);
        EXPECT_EQ(cell_coordinate(binned.z[at], grid), iz);
        if (at > binned.cell_start[cell]) {
          EXPECT_LT(binned.order[at - 1], particle) << "input order within a cell";
        }
      }
    }
    EXPECT_TRUE(std::ranges::all_of(seen, [](bool one) { return one; }));
    for (std::size_t pad = 0; pad < kMaxLanes; ++pad) {
      EXPECT_EQ(binned.x[kParticles + pad], 0.0);
      EXPECT_EQ(binned.y[kParticles + pad], 0.0);
      EXPECT_EQ(binned.z[kParticles + pad], 0.0);
    }
  }
}

TEST(ParticlesBin, CorruptCoordinatesAreBinnedIntoTheNearestCellAndNoParticleIsLost) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> positions = testing::as_doubles(generated_input(5, 0));
  positions[0] = nan;
  positions[1] = -3.0;
  positions[2] = inf;
  positions[3] = 16.0;
  positions[4] = -inf;
  positions[5] = 1e300;
  const Bytes input = testing::as_bytes(positions);
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    Scratch scratch(grid);
    const Binned binned = carve(scratch.span(), scratch.layout, grid);
    sort_into_cells(input, grid, binned);
    EXPECT_EQ(binned.cell_start[grid.cells], kParticles);
    std::vector<std::uint32_t> order(binned.order.begin(), binned.order.end());
    std::ranges::sort(order);
    std::vector<std::uint32_t> all(kParticles);
    std::iota(all.begin(), all.end(), 0U);
    EXPECT_EQ(order, all) << "a permutation";
    // Particle 0 is in cell (0, 0, edge - 1): NaN and negative low, infinity high.
    const std::uint32_t cell0 = (grid.edge - 1) * grid.edge * grid.edge;
    EXPECT_EQ(binned.order[binned.cell_start[cell0]], 0U);
    // Particle 1 is at (16, -inf, 1e300): x, z high and y low.
    const std::uint32_t cell1 = (grid.edge - 1) * grid.edge * grid.edge + grid.edge - 1;
    const auto begin = binned.order.begin() + binned.cell_start[cell1];
    const auto end = binned.order.begin() + binned.cell_start[cell1 + 1];
    EXPECT_NE(std::find(begin, end, 1U), end);
  }
}

TEST(ParticlesBin, AllParticlesAtOnePointShareOneCell) {
  const Bytes input = testing::as_bytes(std::vector<double>(3 * kParticles, 7.25));
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Grid grid = make_grid(c);
    Scratch scratch(grid);
    const Binned binned = carve(scratch.span(), scratch.layout, grid);
    sort_into_cells(input, grid, binned);
    const std::uint32_t cell = cell_coordinate(7.25, grid);
    const std::uint32_t flat = cell + grid.edge * (cell + grid.edge * cell);
    EXPECT_EQ(binned.cell_start[flat], 0U);
    EXPECT_EQ(binned.cell_start[flat + 1], kParticles);
  }
}

// A populated sorted structure to list runs on: the generated input of unit 2.
struct Sorted {
  explicit Sorted(std::uint32_t c) : grid(make_grid(c)), rows(make_row_table(c)), scratch(grid) {
    input = generated_input(21, 2);
    positions = testing::as_doubles(input);
    binned = carve(scratch.span(), scratch.layout, grid);
    sort_into_cells(input, grid, binned);
  }
  Grid grid;
  RowTable rows;
  Scratch scratch;
  Bytes input;
  std::vector<double> positions;
  Binned binned;
};

TEST(ParticlesBin, TheRunsOfACellHoldEveryParticleWithinTheCutoffOfAParticleOfIt) {
  for (const std::uint32_t c : kCellsPerCutoff) {
    const Sorted sorted(c);
    const Grid& grid = sorted.grid;
    std::array<NeighbourRun, kMaxRows> runs{};
    // Cells in the interior, on each face, edge and corner of the box.
    const std::uint32_t last = grid.edge - 1;
    const std::uint32_t mid = grid.edge / 2;
    for (const std::uint32_t ix : {0U, mid, last}) {
      for (const std::uint32_t iy : {0U, mid, last}) {
        for (const std::uint32_t iz : {0U, mid, last}) {
          const std::size_t count =
              collect_runs(grid, sorted.rows, sorted.binned.cell_start, ix, iy, iz, runs);
          ASSERT_LE(count, sorted.rows.count);
          std::vector<bool> listed(kParticles, false);
          for (std::size_t r = 0; r < count; ++r) {
            EXPECT_LT(runs[r].begin, runs[r].end) << "empty runs are left out";
            EXPECT_LE(runs[r].end, kParticles);
            if (r > 0) {
              EXPECT_NE(runs[r].begin, runs[r - 1].begin) << "no run twice";
            }
            for (std::uint32_t at = runs[r].begin; at < runs[r].end; ++at) {
              EXPECT_FALSE(listed[at]) << "no particle in two runs";
              listed[at] = true;
            }
          }
          const std::uint32_t cell = ix + grid.edge * (iy + grid.edge * iz);
          for (std::uint32_t at = sorted.binned.cell_start[cell];
               at < sorted.binned.cell_start[cell + 1]; ++at) {
            for (std::uint32_t other = 0; other < kParticles; ++other) {
              const double dx = sorted.binned.x[at] - sorted.binned.x[other];
              const double dy = sorted.binned.y[at] - sorted.binned.y[other];
              const double dz = sorted.binned.z[at] - sorted.binned.z[other];
              if (dx * dx + dy * dy + dz * dz < kCutoffSquared) {
                ASSERT_TRUE(listed[other]) << "c " << c << " cell " << ix << " " << iy << " " << iz;
              }
            }
          }
        }
      }
    }
  }
}

TEST(ParticlesBin, ARunIsWholeCellsOfOneRowAndTheRunsFollowTheRowTable) {
  Sorted sorted(2);
  const Grid& grid = sorted.grid;
  std::array<NeighbourRun, kMaxRows> runs{};
  const std::uint32_t ix = 6;
  const std::uint32_t iy = 9;
  const std::uint32_t iz = 3;
  const std::size_t count =
      collect_runs(grid, sorted.rows, sorted.binned.cell_start, ix, iy, iz, runs);
  std::size_t next = 0;
  for (std::size_t r = 0; r < sorted.rows.count; ++r) {
    const RowOffset& row = sorted.rows.rows[r];
    const std::uint32_t base =
        static_cast<std::uint32_t>((static_cast<int>(iz) + row.dz) * static_cast<int>(grid.edge) +
                                   static_cast<int>(iy) + row.dy) *
        grid.edge;
    const std::uint32_t begin =
        sorted.binned.cell_start[base + ix - static_cast<std::uint32_t>(row.dx_max)];
    const std::uint32_t end =
        sorted.binned.cell_start[base + ix + static_cast<std::uint32_t>(row.dx_max) + 1];
    if (begin == end) {
      continue;
    }
    ASSERT_LT(next, count);
    EXPECT_EQ(runs[next].begin, begin) << "row " << r;
    EXPECT_EQ(runs[next].end, end) << "row " << r;
    ++next;
  }
  EXPECT_EQ(next, count);
}

TEST(ParticlesBin, ACellOfAnEmptyBoxHasNoRuns) {
  const Bytes input = testing::as_bytes(std::vector<double>(3 * kParticles, 0.0));
  const Grid grid = make_grid(2);
  const RowTable rows = make_row_table(2);
  Scratch scratch(grid);
  const Binned binned = carve(scratch.span(), scratch.layout, grid);
  sort_into_cells(input, grid, binned);
  std::array<NeighbourRun, kMaxRows> runs{};
  EXPECT_EQ(collect_runs(grid, rows, binned.cell_start, 12, 12, 12, runs), 0U) << "far from them";
  EXPECT_EQ(collect_runs(grid, rows, binned.cell_start, 3, 0, 0, runs), 0U)
      << "three cells along x: the row reaches two";
  EXPECT_EQ(collect_runs(grid, rows, binned.cell_start, 2, 0, 0, runs), 1U)
      << "two cells along x reach the origin's cell";
  EXPECT_EQ(collect_runs(grid, rows, binned.cell_start, 1, 1, 1, runs), 1U)
      << "only the row (dy, dz) = (-1, -1) holds the origin's cell";
  EXPECT_EQ(runs[0].begin, 0U);
  EXPECT_EQ(runs[0].end, kParticles);
}

TEST(ParticlesBin, ForcesGoBackToTheSlotOfTheParticleTheyWereSortedFrom) {
  const Grid grid = make_grid(1);
  Scratch scratch(grid);
  const Binned binned = carve(scratch.span(), scratch.layout, grid);
  // A permutation: sorted position k holds particle (k * 1237 + 5) mod n.
  for (std::size_t k = 0; k < kParticles; ++k) {
    binned.order[k] = static_cast<std::uint32_t>((k * 1237 + 5) % kParticles);
    binned.force[3 * k] = static_cast<double>(k);
    binned.force[3 * k + 1] = -static_cast<double>(k);
    binned.force[3 * k + 2] = 0.5 * static_cast<double>(k);
  }
  Bytes output(kOutputBytes, std::byte{0xAB});
  scatter_forces(binned, output);
  const std::vector<double> forces = testing::as_doubles(output);
  for (std::size_t k = 0; k < kParticles; ++k) {
    const std::size_t particle = binned.order[k];
    EXPECT_EQ(forces[3 * particle], static_cast<double>(k));
    EXPECT_EQ(forces[3 * particle + 1], -static_cast<double>(k));
    EXPECT_EQ(forces[3 * particle + 2], 0.5 * static_cast<double>(k));
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
