// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/fft/fft.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "workloads/fft/kernel.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::fft {

namespace {

/// The one ping-pong buffer a unit's stages need, in the first slot's piece
/// of the scratch. The other slots' pieces are not touched: the butterflies of
/// a stage write disjoint elements of one shared buffer, so no task needs a
/// private one.
constexpr std::uint64_t kScratchBytes = kPoints * kElementBytes;

/// Real operations of one unit by the convention 5 n log2 n, the same for both
/// radices so that rates compare across decompositions.
constexpr std::uint64_t kOperations = 5 * kPoints * 12;

class FftWorkload final : public Workload {
 public:
  FftWorkload(Plan plan, std::uint32_t radix, StageKernel kernel, std::uint64_t seed,
              simd::IsaSet paths)
      : descriptor_{std::string(kName), kVersion, DeterminismClass::kBitExactFixedDecomposition,
                    verification::Contract::kBounded, paths},
        plan_(plan),
        radix_(radix),
        kernel_(kernel),
        seed_(seed) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t element = 0; element < 2 * kPoints; ++element) {
      store_double(input, element, entry_value(rng::u64_of(rng::block_at(seed_, index, element))));
    }
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    transform(kernel_, radix_, kPoints, input, output, scratch.first(kScratchBytes), executor);
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint32_t radix_;
  StageKernel kernel_;
  std::uint64_t seed_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t radix = config.decomposition == 0 ? kDefaultRadix : config.decomposition;
  if (!is_radix(radix)) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "fft radix " + std::to_string(radix) + " is not 2 or 4"};
  }
  const simd::KernelTable<StageKernel> kernels = stage_kernels(radix);
  const StageKernel kernel = simd::kernel_at(kernels, config.isa);
  if (kernel == nullptr) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "fft has no " + std::string(simd::describe(config.isa)) + " kernel"};
  }
  const std::uint64_t unit_bytes = kInputBytes + kOutputBytes;
  const std::uint64_t units = config.footprint_bytes / unit_bytes;
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "a footprint of " + std::to_string(config.footprint_bytes) +
                             " bytes holds no fft unit of " + std::to_string(unit_bytes) +
                             " bytes"};
  }
  const Plan plan{kInputBytes, kOutputBytes, kScratchBytes,
                  units,       radix,        std::uint64_t{stage_count(radix)} * 2 * kInputBytes,
                  kOperations};
  // The descriptor is built inside the constructor, so no temporary with a
  // destructor is alive across the allocation (journal 1.13).
  return std::unique_ptr<Workload>(
      std::make_unique<FftWorkload>(plan, radix, kernel, config.seed, simd::kernel_paths(kernels)));
}

}  // namespace loadforge::workloads::fft
