// SPDX-License-Identifier: GPL-3.0-or-later

#include "workloads/dense_linear/dense_linear.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/workload.hpp"

#if defined(LOADFORGE_SIMD_X86_KERNELS)
#include "primitives/simd/x86/gemm_avx2.hpp"
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
#include "primitives/simd/arm/gemm_neon.hpp"
#endif

namespace loadforge::workloads::dense_linear {
namespace {

constexpr std::size_t kMatrixBytes = kOrder * kOrder * sizeof(double);
constexpr std::size_t kElements = kInputBytes / sizeof(double);
constexpr std::uint64_t kUnitBytes = std::uint64_t{kInputBytes} + kOutputBytes;

constexpr Kernels kBuilt{
    .scalar = &panel_scalar,
#if defined(LOADFORGE_SIMD_X86_KERNELS)
    .avx2 = &simd::x86::gemm_panel_avx2,
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
    .neon = &simd::arm::gemm_panel_neon,
#endif
};

/// The paths a set holds, by name, for a person: "scalar, avx2".
[[nodiscard]] std::string names_of(simd::IsaSet paths) {
  std::string names;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, simd::IsaPath::kAvx2,
                                   simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (paths.contains(path)) {
      names += names.empty() ? "" : ", ";
      names += simd::describe(path);
    }
  }
  return names;
}

[[nodiscard]] std::string depths() {
  std::string names;
  for (const std::uint32_t depth : kPanelDepths) {
    names += names.empty() ? "" : ", ";
    names += std::to_string(depth);
  }
  return names;
}

/// What every task of one unit's compute() shares, read-only.
struct UnitWork {
  std::span<const std::byte> a;
  std::span<const std::byte> b;
  std::span<std::byte> c;
  std::size_t depth;
  PanelKernel kernel;
};

/// Task `tile` of a unit: zero the tile's rows of C, then apply every panel in
/// increasing k. The slot is unused: the workload keeps no scratch.
///
/// A named function given to Executor::run rather than a lambda given to
/// for_each_index: the lambda that template wraps around a body takes two
/// adjacent size_t parameters, which clang-tidy refuses in every src/ unit that
/// instantiates it, at a line in a frozen header no NOLINT can be added to
/// (docs/journal/06-workload-dense-linear.md, contract questions).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void run_tile(void* context, std::size_t tile, std::size_t /*slot*/) noexcept {
  const auto& work = *static_cast<const UnitWork*>(context);
  const std::size_t row = tile * kTileRows;
  // +0.0 is the all-zero bit pattern, so the tile's sums start from it.
  std::ranges::fill(
      work.c.subspan(row * kOrder * sizeof(double), kTileRows * kOrder * sizeof(double)),
      std::byte{0});
  for (std::size_t k0 = 0; k0 < kOrder; k0 += work.depth) {
    work.kernel(Panel{work.a, work.b, work.c, row, k0, work.depth});
  }
}

class DenseLinear final : public Workload {
 public:
  DenseLinear(const WorkloadConfig& config, simd::IsaSet paths, PanelKernel kernel,
              std::uint32_t depth, std::uint64_t units)
      : descriptor_{std::string(kName), kVersion, DeterminismClass::kBitExactFixedDecomposition,
                    verification::Contract::kBounded, paths},
        plan_{kInputBytes, kOutputBytes, 0, units, depth, kLogicalBytes, kOperations},
        seed_(config.seed),
        kernel_(kernel) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  /// Element e is entry_value(u64_of(block_at(seed, index, e))), A's n^2
  /// first, then B's, each written as its eight little-endian bytes.
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t element = 0; element < kElements; ++element) {
      const double value = entry_value(rng::u64_of(rng::block_at(seed_, index, element)));
      const auto bits = std::bit_cast<std::uint64_t>(value);
      for (std::size_t byte = 0; byte < sizeof bits; ++byte) {
        input[element * sizeof bits + byte] = static_cast<std::byte>(bits >> (8U * byte));
      }
    }
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& executor) const noexcept override {
    const std::span<const std::byte> a = input.first(kMatrixBytes);
    const std::span<const std::byte> b = input.subspan(kMatrixBytes, kMatrixBytes);
    UnitWork work{a, b, output, plan_.decomposition, kernel_};
    executor.run(kTiles, &run_tile, &work);
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
  PanelKernel kernel_;
};

}  // namespace

const Kernels& built_kernels() noexcept { return kBuilt; }

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  return create(config, kBuilt);
}

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config,
                                                              const Kernels& kernels) {
  const simd::IsaSet paths = simd::kernel_paths(kernels);
  if (!paths.contains(simd::IsaPath::kScalar)) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "the kernel table has no scalar kernel, which every table must hold"};
  }
  const std::uint32_t depth = config.decomposition == 0 ? kDefaultPanelDepth : config.decomposition;
  if (!is_panel_depth(depth)) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "panel depth " + std::to_string(depth) + " is not one of " + depths()};
  }
  const PanelKernel kernel = simd::kernel_at(kernels, config.isa);
  if (kernel == nullptr) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "no dense_linear kernel for ISA path " +
                             std::string(simd::describe(config.isa)) + "; this build has " +
                             names_of(paths)};
  }
  const std::uint64_t units = config.footprint_bytes / kUnitBytes;
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "a footprint of " + std::to_string(config.footprint_bytes) +
                             " bytes holds no unit of " + std::to_string(kUnitBytes) + " bytes"};
  }
  return std::unique_ptr<Workload>(
      std::make_unique<DenseLinear>(config, paths, kernel, depth, units));
}

}  // namespace loadforge::workloads::dense_linear
