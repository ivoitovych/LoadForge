// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_BIN_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_BIN_HPP

// The neighbour search: sort the particles into cells, list for a cell the
// runs of particles near it, and put the forces back in the input's order.
// The scratch holds everything (grid.hpp, Layout); nothing is allocated.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/particles/grid.hpp"
#include "workloads/particles/kernel.hpp"

namespace loadforge::workloads::particles {

/// The scratch, carved as the layout says.
struct Binned {
  std::span<std::uint32_t> cell_start;
  std::span<std::uint32_t> cell_of;
  std::span<std::uint32_t> order;
  std::span<double> x;
  std::span<double> y;
  std::span<double> z;
  std::span<double> force;
};

/// Precondition: scratch.size() >= layout.total + kScratchAlignment (the
/// carving starts at the first aligned address in it).
[[nodiscard]] Binned carve(std::span<std::byte> scratch, const Layout& layout,
                           const Grid& grid) noexcept;

/// Reads the kParticles positions of `input` (3 little-endian doubles each)
/// and fills cell_start, order and the sorted coordinates, with
/// kMaxLanes zeros after each coordinate array. Within a cell the particles
/// keep their input order. Every entry the search reads is written here, so
/// nothing the scratch held before matters.
void sort_into_cells(std::span<const std::byte> input, const Grid& grid,
                     const Binned& binned) noexcept;

/// The runs of sorted particles that the cell (ix, iy, iz) must sum over, in
/// the order of the row table; empty runs are left out. Returns their count.
[[nodiscard]] std::size_t collect_runs(const Grid& grid, const RowTable& rows,
                                       std::span<const std::uint32_t> cell_start, std::uint32_t ix,
                                       std::uint32_t iy, std::uint32_t iz,
                                       std::array<Run, kMaxRows>& runs) noexcept;

/// Writes the force at sorted position k to the output slot of particle
/// order[k]: every output byte is written once.
void scatter_forces(const Binned& binned, std::span<std::byte> output) noexcept;

}  // namespace loadforge::workloads::particles

#endif
