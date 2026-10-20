// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/stencil/stencil.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "primitives/rng/philox.hpp"
#include "worker/executor.hpp"
#include "workloads/stencil/kernels.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {

namespace {

static_assert(std::endian::native == std::endian::little,
              "grids are little-endian doubles and are read and written in place");

constexpr std::size_t kCellBytes = sizeof(double);
constexpr std::size_t kInteriorEdge = kEdge - 2;
constexpr std::uint64_t kFlopsPerCell = 6;  // three additions, two multiplications, one more

/// One sweep, as the executor's tasks see it: tile `index` of the grid, read
/// from `src`, written to `dst`.
struct Sweep {
  std::span<const std::byte> src;
  std::span<std::byte> dst;
  SweepKernel kernel = nullptr;
  std::size_t tile = 0;
  std::size_t tiles_per_side = 0;
};

void copy_cells(const Sweep& sweep, std::size_t first_cell, std::size_t count) noexcept {
  std::memcpy(sweep.dst.subspan(first_cell * kCellBytes, count * kCellBytes).data(),
              sweep.src.subspan(first_cell * kCellBytes, count * kCellBytes).data(),
              count * kCellBytes);
}

/// The tile's interior cells by the kernel, and the ring cells it covers
/// copied unchanged. Reads only `src`, writes only the tile's cells of `dst`.
void sweep_tile(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  const Sweep& sweep = *static_cast<const Sweep*>(context);
  const std::size_t row_begin = (index / sweep.tiles_per_side) * sweep.tile;
  const std::size_t column_begin = (index % sweep.tiles_per_side) * sweep.tile;
  const std::size_t row_end = row_begin + sweep.tile;
  const std::size_t column_end = column_begin + sweep.tile;

  sweep.kernel(sweep.src, sweep.dst,
               Rect{std::max<std::size_t>(row_begin, 1), std::min(row_end, kEdge - 1),
                    std::max<std::size_t>(column_begin, 1), std::min(column_end, kEdge - 1)});

  for (std::size_t row = row_begin; row < row_end; ++row) {
    if (row == 0 || row == kEdge - 1) {
      copy_cells(sweep, (row * kEdge) + column_begin, sweep.tile);
      continue;
    }
    if (column_begin == 0) {
      copy_cells(sweep, row * kEdge, 1);
    }
    if (column_end == kEdge) {
      copy_cells(sweep, (row * kEdge) + kEdge - 1, 1);
    }
  }
}

class Stencil final : public Workload {
 public:
  Stencil(const WorkloadConfig& config, std::uint32_t tile_edge, SweepKernel kernel,
          std::uint64_t units, simd::IsaSet paths)
      : descriptor_{std::string(kName), kVersion, DeterminismClass::kBitExactFixedDecomposition,
                    verification::Contract::kBounded, paths},
        plan_{kInputBytes,
              kOutputBytes,
              kGridBytes,
              units,
              tile_edge,
              std::uint64_t{kSteps} * 2 * kGridBytes,
              std::uint64_t{kSteps} * kInteriorEdge * kInteriorEdge * kFlopsPerCell},
        seed_(config.seed),
        kernel_(kernel) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
      const double value = entry_value(rng::u64_of(rng::block_at(seed_, index, cell)));
      std::memcpy(input.subspan(cell * kCellBytes, kCellBytes).data(), &value, kCellBytes);
    }
  }

  /// Sweeps alternate between the scratch grid and the output: the first reads
  /// the input and writes scratch, the last (kSteps is even) writes the output.
  /// A sweep is one executor run, so the run's return is the barrier between
  /// sweeps; within one, tiles share nothing but the read-only source.
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    static_assert(kSteps % 2 == 0, "the last sweep must land in the output");
    const std::span<std::byte> middle = scratch.first(kGridBytes);
    const std::size_t tiles_per_side = kEdge / plan_.decomposition;
    for (std::size_t step = 0; step < kSteps; ++step) {
      Sweep sweep;
      sweep.src = step == 0 ? input
                            : (step % 2 == 1 ? std::span<const std::byte>(middle)
                                             : std::span<const std::byte>(output));
      sweep.dst = step % 2 == 0 ? middle : output;
      sweep.kernel = kernel_;
      sweep.tile = plan_.decomposition;
      sweep.tiles_per_side = tiles_per_side;
      executor.run(tiles_per_side * tiles_per_side, sweep_tile, &sweep);
    }
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
  SweepKernel kernel_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t tile_edge =
      config.decomposition == 0 ? kDefaultTileEdge : config.decomposition;
  if (!is_tile_edge(tile_edge)) {
    return WorkloadError{
        WorkloadFault::kConfiguration,
        "stencil tile edge " + std::to_string(tile_edge) + " is not one of 16, 32, 64, 128"};
  }
  const simd::KernelTable<SweepKernel> table = kernel_table();
  const SweepKernel kernel = simd::kernel_at(table, config.isa);
  if (kernel == nullptr) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "stencil has no kernel for the " +
                             std::string(simd::describe(config.isa)) + " path in this build"};
  }
  const std::uint64_t units = config.footprint_bytes / (kInputBytes + kOutputBytes);
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "a footprint of " + std::to_string(config.footprint_bytes) +
                             " bytes holds no stencil unit (" +
                             std::to_string(kInputBytes + kOutputBytes) + " bytes each)"};
  }
  return std::unique_ptr<Workload>(
      std::make_unique<Stencil>(config, tile_edge, kernel, units, simd::kernel_paths(table)));
}

}  // namespace loadforge::workloads::stencil
