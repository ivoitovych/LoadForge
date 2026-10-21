// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/particles/particles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/particles/bin.hpp"
#include "workloads/particles/grid.hpp"
#include "workloads/particles/kernel.hpp"
#include "workloads/particles/kernels.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles {
namespace {

/// Floating-point operations one candidate pair costs in the model of the
/// specification: 3 differences, 3 squares and 2 sums for q, a product and a
/// difference for t, a square for w, 3 products and 3 sums into the force.
constexpr std::uint64_t kOperationsPerPair = 17;

/// What the tasks of one unit share; read-only while they run.
struct Sum {
  const Grid* grid = nullptr;
  const RowTable* rows = nullptr;
  const Binned* binned = nullptr;
  CellKernel kernel = nullptr;
};

/// Task `row`: the cells of one row (fixed y and z) of the grid.
void sum_row(void* context, std::size_t row, std::size_t /*slot*/) noexcept {
  const Sum& sum = *static_cast<const Sum*>(context);
  const Binned& binned = *sum.binned;
  const std::uint32_t edge = sum.grid->edge;
  const auto iz = static_cast<std::uint32_t>(row / edge);
  const auto iy = static_cast<std::uint32_t>(row % edge);
  std::array<NeighbourRun, kMaxRows> runs{};
  for (std::uint32_t ix = 0; ix < edge; ++ix) {
    const std::size_t cell = (static_cast<std::size_t>(iz) * edge + iy) * edge + ix;
    const std::uint32_t first = binned.cell_start[cell];
    const std::uint32_t last = binned.cell_start[cell + 1];
    if (first == last) {
      continue;
    }
    CellJob job;
    job.x = binned.x.data();
    job.y = binned.y.data();
    job.z = binned.z.data();
    job.run_count = collect_runs(*sum.grid, *sum.rows, binned.cell_start, ix, iy, iz, runs);
    job.runs = runs.data();
    job.first = first;
    job.last = last;
    job.force = binned.force.data();
    sum.kernel(job);
  }
}

class ParticlesWorkload final : public Workload {
 public:
  ParticlesWorkload(const WorkloadConfig& config, const Grid& grid, CellKernel kernel,
                    const Plan& plan, Descriptor descriptor)
      : descriptor_(std::move(descriptor)),
        plan_(plan),
        seed_(config.seed),
        grid_(grid),
        rows_(make_row_table(grid.cells_per_cutoff)),
        layout_(make_layout(grid)),
        kernel_(kernel) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::uint64_t element = 0; element < 3 * kParticles; ++element) {
      const double value = position_value(rng::u64_of(rng::block_at(seed_, index, element)));
      std::memcpy(input.data() + element * sizeof(double), &value, sizeof(double));
    }
  }

  // The signature is the contract's.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const Binned binned = carve(scratch, layout_, grid_);
    sort_into_cells(input, grid_, binned);
    Sum sum;
    sum.grid = &grid_;
    sum.rows = &rows_;
    sum.binned = &binned;
    sum.kernel = kernel_;
    executor.run(static_cast<std::size_t>(grid_.edge) * grid_.edge, &sum_row, &sum);
    scatter_forces(binned, output);
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
  Grid grid_;
  RowTable rows_;
  Layout layout_;
  CellKernel kernel_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t cells =
      config.decomposition == 0 ? kDefaultCellsPerCutoff : config.decomposition;
  if (!is_cells_per_cutoff(cells)) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "cells per cutoff " + std::to_string(cells) + " is not 1, 2 or 4"};
  }
  const simd::KernelTable<CellKernel>& table = cell_kernels();
  const CellKernel kernel = simd::kernel_at(table, config.isa);
  if (kernel == nullptr) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "no particles kernel for ISA path " +
                             std::string(simd::describe(config.isa)) + " in this build"};
  }
  const std::uint64_t unit_bytes = kInputBytes + kOutputBytes;
  const std::uint64_t units = config.footprint_bytes / unit_bytes;
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "a footprint of " + std::to_string(config.footprint_bytes) +
                             " bytes holds no unit of " + std::to_string(unit_bytes) + " bytes"};
  }
  const Grid grid = make_grid(cells);
  const Layout layout = make_layout(grid);
  const RowTable rows = make_row_table(cells);
  std::uint64_t visited_cells = 0;
  for (const RowOffset& row : std::span(rows.rows).first(rows.count)) {
    visited_cells += 2U * static_cast<std::uint64_t>(row.dx_max) + 1U;
  }
  Plan plan;
  plan.input_bytes = kInputBytes;
  plan.output_bytes = kOutputBytes;
  plan.scratch_bytes = layout.total + kScratchAlignment;
  plan.units = units;
  plan.decomposition = cells;
  // Input and output once, the scratch written once and read once.
  plan.logical_bytes = kInputBytes + kOutputBytes + 2 * layout.total;
  // Candidate pairs: each particle meets the particles of the cells it looks
  // at, kParticles / grid.cells of them in a cell on average.
  plan.operations = kParticles * kParticles * visited_cells / grid.cells * kOperationsPerPair;
  Descriptor descriptor{std::string(kName), kVersion, DeterminismClass::kBitExactFixedDecomposition,
                        verification::Contract::kBounded, simd::kernel_paths(table)};
  return std::unique_ptr<Workload>(
      std::make_unique<ParticlesWorkload>(config, grid, kernel, plan, std::move(descriptor)));
}

}  // namespace loadforge::workloads::particles
