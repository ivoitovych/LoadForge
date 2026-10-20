// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/raytrace/raytrace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/raytrace/generator.hpp"
#include "workloads/raytrace/kernels.hpp"
#include "workloads/raytrace/scene.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::raytrace {
namespace {

/// What one executor task needs to trace a row. Read-only for the tasks; each
/// writes only its own row of the output.
struct RowContext {
  const SceneStorage* scene;
  RowKernel kernel;
  std::byte* records;
};

void trace_one_row(void* context, std::size_t row, std::size_t /*slot*/) noexcept {
  const auto& job = *static_cast<const RowContext*>(context);
  const std::span<std::byte, kRowBytes> out(job.records + row * kRowBytes, kRowBytes);
  job.kernel(*job.scene, static_cast<std::uint32_t>(row), out);
}

/// Tile sum k, for tiles of `edge` pixels a side: the spec's plain
/// left-to-right sum of the t of each hit, rows from the top.
[[nodiscard]] double tile_sum(std::span<const std::byte> records, std::uint32_t edge,
                              std::size_t tile) {
  const std::size_t per_row = kWidth / edge;
  const std::size_t first_x = (tile % per_row) * edge;
  const std::size_t first_y = (tile / per_row) * edge;
  double sum = 0.0;
  for (std::size_t y = first_y; y < first_y + edge; ++y) {
    for (std::size_t x = first_x; x < first_x + edge; ++x) {
      const std::size_t at = pixel_offset(y * kWidth + x);
      std::uint32_t id = 0;
      std::memcpy(&id, records.data() + at + sizeof(double), sizeof(id));
      if (id != kNoHit) {
        double t = 0.0;
        std::memcpy(&t, records.data() + at, sizeof(t));
        sum = sum + t;
      }
    }
  }
  return sum;
}

class RaytraceWorkload final : public Workload {
 public:
  RaytraceWorkload(Descriptor descriptor, Plan plan, RowKernel kernel, std::uint64_t seed)
      : descriptor_(std::move(descriptor)), plan_(plan), kernel_(kernel), seed_(seed) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    generate_scene(seed_, index, input.first<kInputBytes>());
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& executor) const noexcept override {
    // The scene and its hierarchy live on this thread's stack for the length
    // of the call: the tasks only read them, and run() returns after the last.
    SceneStorage scene;
    decode_scene(input.first<kInputBytes>(), scene);
    build_bvh(scene, kBoundMargin);

    RowContext context{&scene, kernel_, output.data()};
    executor.run(kHeight, &trace_one_row, &context);

    const std::uint32_t edge = plan_.decomposition;
    const std::span<const std::byte> records = output.first(kTileSumsOffset);
    for (std::size_t tile = 0; tile < kMaxTiles; ++tile) {
      const double sum = tile < tile_count(edge) ? tile_sum(records, edge, tile) : 0.0;
      std::memcpy(output.data() + tile_sum_offset(tile), &sum, sizeof(sum));
    }
  }


 private:
  Descriptor descriptor_;
  Plan plan_;
  RowKernel kernel_;
  std::uint64_t seed_;
};

[[nodiscard]] WorkloadError refuse(WorkloadFault fault, std::string detail) {
  WorkloadError error;
  error.fault = fault;
  error.detail = std::move(detail);
  return error;
}

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t edge = config.decomposition == 0 ? kDefaultTileEdge : config.decomposition;
  if (!is_tile_edge(edge)) {
    return refuse(WorkloadFault::kConfiguration,
                  "tile edge " + std::to_string(config.decomposition) + " is not one of 8, 16, 32");
  }
  const simd::KernelTable<RowKernel> table = row_kernels();
  const RowKernel kernel = simd::kernel_at(table, config.isa);
  if (kernel == nullptr) {
    return refuse(WorkloadFault::kConfiguration,
                  "no ray tracing kernel for the " + std::string(simd::describe(config.isa)) +
                      " path in this build");
  }
  constexpr std::uint64_t kUnitBytes = kInputBytes + kOutputBytes;
  const std::uint64_t units = config.footprint_bytes / kUnitBytes;
  if (units == 0) {
    return refuse(WorkloadFault::kFootprintTooSmall,
                  "a unit takes " + std::to_string(kUnitBytes) + " bytes, the footprint is " +
                      std::to_string(config.footprint_bytes));
  }

  Descriptor descriptor;
  descriptor.name = std::string(kName);
  descriptor.version = kVersion;
  descriptor.determinism = DeterminismClass::kBitExactFixedDecomposition;
  descriptor.contract = verification::Contract::kExact;
  descriptor.paths = simd::kernel_paths(table);

  Plan plan;
  plan.input_bytes = kInputBytes;
  plan.output_bytes = kOutputBytes;
  plan.scratch_bytes = 0;
  plan.units = units;
  plan.decomposition = edge;
  // The scene is read once, the records written once and read again for the
  // tile sums.
  plan.logical_bytes = kInputBytes + kOutputBytes + kPixels * sizeof(double);
  // One operation is one ray against one primitive, resolved: every pair, as
  // brute force would count them, however few the hierarchy tries.
  plan.operations = kPixels * kPrimitives;

  return std::unique_ptr<Workload>(
      std::make_unique<RaytraceWorkload>(std::move(descriptor), plan, kernel, config.seed));
}

}  // namespace loadforge::workloads::raytrace
