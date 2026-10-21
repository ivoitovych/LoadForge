// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/monte_carlo/monte_carlo.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/monte_carlo/kernels.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::monte_carlo {

namespace {

constexpr std::size_t kWordBytes = sizeof(std::uint64_t);

void store_word(std::span<std::byte> bytes, std::uint64_t value) noexcept {
  for (std::size_t byte = 0; byte < kWordBytes; ++byte) {
    bytes[byte] = static_cast<std::byte>((value >> (8U * byte)) & 0xFFU);
  }
}

std::uint64_t load_word(std::span<const std::byte> bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < kWordBytes; ++byte) {
    value |= std::uint64_t{static_cast<std::uint8_t>(bytes[byte])} << (8U * byte);
  }
  return value;
}

/// T_b: the chunk sums of a batch, added in increasing chunk number from
/// chunk 0's sum (spec.hpp, DECOMPOSITION).
double batch_sum(ChunkKernel kernel, std::uint64_t key, std::uint32_t chunk) noexcept {
  double total = kernel(key, 0, chunk);
  for (std::uint64_t first = chunk; first < kSamplesPerBatch; first += chunk) {
    total += kernel(key, first, chunk);
  }
  return total;
}

/// What one run of compute() hands each task: the unit's spans and the
/// kernel and chunk length in use. Tasks write disjoint eight-byte words.
struct BatchJob {
  ChunkKernel kernel;
  std::uint32_t chunk;
  std::span<const std::byte> input;
  std::span<std::byte> output;
};

void run_batch(void* context, std::size_t batch, std::size_t /*slot*/) noexcept {
  const BatchJob& job = *static_cast<const BatchJob*>(context);
  const std::size_t offset = batch * kWordBytes;
  const std::uint64_t key = load_word(job.input.subspan(offset, kWordBytes));
  const double sum = batch_sum(job.kernel, key, job.chunk);
  store_word(job.output.subspan(offset, kWordBytes), std::bit_cast<std::uint64_t>(sum));
}

class MonteCarlo final : public Workload {
 public:
  MonteCarlo(const WorkloadConfig& config, Descriptor descriptor, const Plan& plan,
             ChunkKernel kernel) noexcept
      : seed_(config.seed), descriptor_(std::move(descriptor)), plan_(plan), kernel_(kernel) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t batch = 0; batch < kBatches; ++batch) {
      store_word(input.subspan(batch * kWordBytes, kWordBytes), batch_key(seed_, index, batch));
    }
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& executor) const noexcept override {
    BatchJob job{kernel_, plan_.decomposition, input, output};
    executor.run(kBatches, &run_batch, &job);
  }

 private:
  std::uint64_t seed_;
  Descriptor descriptor_;
  Plan plan_;
  ChunkKernel kernel_;
};

WorkloadError refuse(WorkloadFault fault, std::string detail) noexcept {
  return WorkloadError{fault, std::move(detail)};
}

}  // namespace

Descriptor workload_descriptor() {
  Descriptor descriptor;
  descriptor.name = std::string(kName);
  descriptor.version = kVersion;
  descriptor.determinism = DeterminismClass::kBitExactFixedDecomposition;
  descriptor.contract = verification::Contract::kBounded;
  descriptor.paths = simd::kernel_paths(chunk_kernels());
  return descriptor;
}

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const simd::KernelTable<ChunkKernel> kernels = chunk_kernels();
  const ChunkKernel kernel = simd::kernel_at(kernels, config.isa);
  if (kernel == nullptr) {
    return refuse(
        WorkloadFault::kConfiguration,
        "monte_carlo has no " + std::string(simd::describe(config.isa)) + " kernel in this build");
  }
  const std::uint32_t chunk =
      config.decomposition == 0 ? kDefaultChunkLength : config.decomposition;
  if (!is_chunk_length(chunk)) {
    return refuse(WorkloadFault::kConfiguration,
                  "decomposition " + std::to_string(config.decomposition) +
                      " is not a chunk length monte_carlo has (0 asks for " +
                      std::to_string(kDefaultChunkLength) + ")");
  }
  constexpr std::uint64_t kUnitBytes = kInputBytes + kOutputBytes;
  if (config.footprint_bytes < kUnitBytes) {
    return refuse(WorkloadFault::kFootprintTooSmall,
                  "footprint " + std::to_string(config.footprint_bytes) +
                      " bytes holds no unit; one unit takes " + std::to_string(kUnitBytes));
  }
  Plan plan;
  plan.input_bytes = kInputBytes;
  plan.output_bytes = kOutputBytes;
  plan.scratch_bytes = 0;
  plan.units = config.footprint_bytes / kUnitBytes;
  plan.decomposition = chunk;
  plan.logical_bytes = kUnitBytes;
  plan.operations = std::uint64_t{kBatches} * kSamplesPerBatch;
  return std::unique_ptr<Workload>(
      std::make_unique<MonteCarlo>(config, workload_descriptor(), plan, kernel));
}

}  // namespace loadforge::workloads::monte_carlo
