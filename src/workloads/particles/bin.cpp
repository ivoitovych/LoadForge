// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/particles/bin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles {

static_assert(std::endian::native == std::endian::little,
              "the unit's bytes are little-endian doubles, read in place");

namespace {

constexpr std::size_t kPositionBytes = 3 * sizeof(double);

std::array<double, 3> position_of(std::span<const std::byte> input, std::size_t particle) noexcept {
  std::array<double, 3> position{};
  std::memcpy(position.data(), input.data() + particle * kPositionBytes, kPositionBytes);
  return position;
}

std::uint32_t cell_of_position(const std::array<double, 3>& position, const Grid& grid) noexcept {
  return cell_coordinate(position[0], grid) +
         grid.edge * (cell_coordinate(position[1], grid) + grid.edge * cell_coordinate(position[2], grid));
}

template <typename T>
std::span<T> typed_span(std::byte* base, std::size_t offset, std::size_t count) noexcept {
  // The scratch is byte storage the runner owns; the layout puts each array
  // at an offset aligned for its type, so the cast yields an aligned pointer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<T*>(base + offset), count};
}

}  // namespace

Binned carve(std::span<std::byte> scratch, const Layout& layout, const Grid& grid) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto address = reinterpret_cast<std::uintptr_t>(scratch.data());
  const std::size_t skip = (kScratchAlignment - address % kScratchAlignment) % kScratchAlignment;
  std::byte* const base = scratch.data() + skip;
  constexpr std::size_t kPadded = kParticles + kMaxLanes;
  Binned binned;
  binned.cell_start = typed_span<std::uint32_t>(base, layout.cell_start, grid.cells + 1U);
  binned.cell_of = typed_span<std::uint32_t>(base, layout.cell_of, kParticles);
  binned.order = typed_span<std::uint32_t>(base, layout.order, kParticles);
  binned.x = typed_span<double>(base, layout.x, kPadded);
  binned.y = typed_span<double>(base, layout.y, kPadded);
  binned.z = typed_span<double>(base, layout.z, kPadded);
  binned.force = typed_span<double>(base, layout.force, 3 * kParticles);
  return binned;
}

void sort_into_cells(std::span<const std::byte> input, const Grid& grid,
                     const Binned& binned) noexcept {
  std::ranges::fill(binned.cell_start, 0U);
  for (std::size_t particle = 0; particle < kParticles; ++particle) {
    const std::uint32_t cell = cell_of_position(position_of(input, particle), grid);
    binned.cell_of[particle] = cell;
    ++binned.cell_start[cell];
  }
  // Counts to first positions; the scatter below moves each to the cell's end.
  std::uint32_t running = 0;
  for (std::uint32_t cell = 0; cell < grid.cells; ++cell) {
    const std::uint32_t count = binned.cell_start[cell];
    binned.cell_start[cell] = running;
    running += count;
  }
  for (std::size_t particle = 0; particle < kParticles; ++particle) {
    const std::array<double, 3> position = position_of(input, particle);
    const std::uint32_t at = binned.cell_start[binned.cell_of[particle]]++;
    binned.order[at] = static_cast<std::uint32_t>(particle);
    binned.x[at] = position[0];
    binned.y[at] = position[1];
    binned.z[at] = position[2];
  }
  // Each entry is now the end of its cell, which is the start of the next.
  for (std::uint32_t cell = grid.cells; cell > 0; --cell) {
    binned.cell_start[cell] = binned.cell_start[cell - 1];
  }
  binned.cell_start[0] = 0;
  for (std::size_t pad = 0; pad < kMaxLanes; ++pad) {
    binned.x[kParticles + pad] = 0.0;
    binned.y[kParticles + pad] = 0.0;
    binned.z[kParticles + pad] = 0.0;
  }
}

std::size_t collect_runs(const Grid& grid, const RowTable& rows,
                         std::span<const std::uint32_t> cell_start, std::uint32_t ix,
                         std::uint32_t iy, std::uint32_t iz,
                         std::array<Run, kMaxRows>& runs) noexcept {
  const auto edge = static_cast<std::int32_t>(grid.edge);
  std::size_t count = 0;
  for (std::size_t r = 0; r < rows.count; ++r) {
    const RowOffset& row = rows.rows[r];
    const std::int32_t y = static_cast<std::int32_t>(iy) + row.dy;
    const std::int32_t z = static_cast<std::int32_t>(iz) + row.dz;
    if (y < 0 || y >= edge || z < 0 || z >= edge) {
      continue;
    }
    const std::int32_t low = std::max(static_cast<std::int32_t>(ix) - row.dx_max, 0);
    const std::int32_t high = std::min(static_cast<std::int32_t>(ix) + row.dx_max, edge - 1);
    const auto base = static_cast<std::size_t>((z * edge + y) * edge);
    const std::uint32_t begin = cell_start[base + static_cast<std::size_t>(low)];
    const std::uint32_t end = cell_start[base + static_cast<std::size_t>(high) + 1];
    if (begin == end) {
      continue;
    }
    runs[count] = Run{begin, end};
    ++count;
  }
  return count;
}

void scatter_forces(const Binned& binned, std::span<std::byte> output) noexcept {
  for (std::size_t at = 0; at < kParticles; ++at) {
    std::memcpy(output.data() + binned.order[at] * kPositionBytes, binned.force.data() + 3 * at,
                kPositionBytes);
  }
}

}  // namespace loadforge::workloads::particles
