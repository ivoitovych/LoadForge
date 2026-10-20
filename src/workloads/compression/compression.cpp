// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/compression/compression.hpp"

#include <algorithm>
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
#include "workloads/compression/generator.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::compression {
namespace {

void store_le32(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) noexcept {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    bytes[at + byte] = static_cast<std::byte>((value >> (8U * byte)) & 0xFFU);
  }
}

class Compression final : public Workload {
 public:
  Compression(const WorkloadConfig& config, const Plan& plan)
      : descriptor_{std::string(kName), kVersion, DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(config.seed) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    generate_input(seed_, index, input);
  }

  // One task: the executor decides which thread compresses the unit, and that
  // thread's slot decides which piece of scratch holds its table. Neither can
  // change the block, which is what the T6 tests hold it to.
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    auto compress = [input, output, scratch](std::size_t /*task*/, std::size_t slot) noexcept {
      write_unit(input, output, scratch.subspan(slot * kTableBytes, kTableBytes));
    };
    worker::for_each_index(executor, 1, compress);
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
};

}  // namespace

void write_unit(std::span<const std::byte> input, std::span<std::byte> output,
                std::span<std::byte> table) noexcept {
  const std::size_t length = compress_block(input, output.subspan(kHeaderBytes), table);
  store_le32(output, 0, kMagic);
  store_le32(output, 4, static_cast<std::uint32_t>(length));
  std::ranges::fill(output.subspan(kHeaderBytes + length), std::byte{0});
}

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  if (config.isa != simd::IsaPath::kScalar) {
    std::string detail = "no kernel for the " + std::string(simd::describe(config.isa)) +
                         " path; compression has the scalar kernel only";
    return WorkloadError{WorkloadFault::kConfiguration, std::move(detail)};
  }
  if (config.decomposition > 1) {
    std::string detail = "decomposition " + std::to_string(config.decomposition) +
                         ": compression has none, and accepts only 0 or 1";
    return WorkloadError{WorkloadFault::kConfiguration, std::move(detail)};
  }
  const std::uint64_t input_bytes = kInputBytes;
  const std::uint64_t output_bytes = output_bytes_for(kInputBytes);
  const std::uint64_t units = config.footprint_bytes / (input_bytes + output_bytes);
  if (units == 0) {
    std::string detail = std::to_string(config.footprint_bytes) +
                         " bytes hold no unit; one takes " +
                         std::to_string(input_bytes + output_bytes);
    return WorkloadError{WorkloadFault::kFootprintTooSmall, std::move(detail)};
  }
  const Plan plan{input_bytes, output_bytes, kTableBytes, units, 1, input_bytes + output_bytes,
                  input_bytes};
  return std::unique_ptr<Workload>(std::make_unique<Compression>(config, plan));
}

}  // namespace loadforge::workloads::compression
