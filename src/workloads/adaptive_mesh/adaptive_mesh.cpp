// SPDX-License-Identifier: GPL-3.0-or-later

#include "workloads/adaptive_mesh/adaptive_mesh.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::adaptive_mesh {
namespace {

constexpr std::uint64_t kUnitBytes = std::uint64_t{kInputBytes} + kOutputBytes;

/// A leaf of the mesh, and the coarse cell it is gathered into. Packed to
/// eight bytes: every field is below 512.
struct Leaf {
  std::uint16_t cell;
  std::uint16_t ix;
  std::uint16_t iy;
  std::uint8_t level;
};

/// The pending cells of one coarse cell's depth-first walk: each split pops
/// one cell and pushes four, over kMaxLevel - kBaseLevel levels.
constexpr std::size_t kStackCells = 3 * (kMaxLevel - kBaseLevel) + 1;

using Hotspots = std::array<Hotspot, kHotspots>;

[[nodiscard]] std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < sizeof value; ++byte) {
    value |= static_cast<std::uint32_t>(bytes[at + byte]) << (8U * byte);
  }
  return value;
}

void store_u32(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) noexcept {
  for (std::size_t byte = 0; byte < sizeof value; ++byte) {
    bytes[at + byte] = static_cast<std::byte>(value >> (8U * byte));
  }
}

void store_u64(std::span<std::byte> bytes, std::size_t at, std::uint64_t value) noexcept {
  for (std::size_t byte = 0; byte < sizeof value; ++byte) {
    bytes[at + byte] = static_cast<std::byte>(value >> (8U * byte));
  }
}

[[nodiscard]] bool splits(const Cell& cell, const Hotspots& hotspots) noexcept {
  return cell.level < kMaxLevel && std::ranges::any_of(hotspots, [&cell](const Hotspot& hotspot) {
           return cell_contains(cell, hotspot);
         });
}

/// Walks the coarse cells in index order, each depth first with the children
/// of a split cell in the order q = 0..3, and appends every leaf to `leaves`
/// and its count to `counts`. Returns the number of leaves. The table holds
/// kMaxLeaves, which no input can exceed (adaptive_mesh.hpp).
[[nodiscard]] std::size_t build_mesh(const Hotspots& hotspots, std::span<Leaf> leaves,
                                     std::span<std::uint32_t> counts) noexcept {
  std::size_t made = 0;
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    std::array<Cell, kStackCells> pending{};
    std::size_t top = 0;
    pending[top++] = Cell{kBaseLevel, static_cast<std::uint32_t>(cell % kBaseCells),
                          static_cast<std::uint32_t>(cell / kBaseCells)};
    const std::size_t before = made;
    while (top > 0) {
      const Cell next = pending[--top];
      if (splits(next, hotspots)) {
        for (std::uint32_t q = 4; q-- > 0;) {
          pending[top++] = Cell{next.level + 1, (2 * next.ix) + (q & 1U), (2 * next.iy) + (q >> 1U)};
        }
      } else {
        leaves[made++] = Leaf{static_cast<std::uint16_t>(cell), static_cast<std::uint16_t>(next.ix),
                              static_cast<std::uint16_t>(next.iy),
                              static_cast<std::uint8_t>(next.level)};
      }
    }
    counts[cell] = static_cast<std::uint32_t>(made - before);
  }
  return made;
}

/// What every task of one unit's compute() shares, read-only except the
/// scratch, of which each slot writes only its own piece.
struct UnitWork {
  const Leaf* leaves;
  const Hotspots* hotspots;
  std::byte* scratch;
};

/// Task `index` of a unit: value the leaf and add it to the partial sum of
/// the slot that claimed it, for the leaf's coarse cell. The value is the
/// fixed sequence of spec.hpp: the terms added in hotspot order from +0.0.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void run_leaf(void* context, std::size_t index, std::size_t slot) noexcept {
  const auto& work = *static_cast<const UnitWork*>(context);
  const Leaf& leaf = work.leaves[index];
  const Point point{cell_centre(leaf.level, leaf.ix), cell_centre(leaf.level, leaf.iy)};
  double sum = 0.0;
  for (const Hotspot& hotspot : *work.hotspots) {
    sum += hotspot_term(point, hotspot);
  }
  const double value = sum * leaf_area(leaf.level);
  // The scratch has no alignment promise, so the partial sum moves by memcpy.
  std::byte* partial = work.scratch + (slot * kScratchBytes) + (leaf.cell * sizeof(double));
  double held = 0.0;
  std::memcpy(&held, partial, sizeof held);
  held += value;
  std::memcpy(partial, &held, sizeof held);
}

class AdaptiveMesh final : public Workload {
 public:
  AdaptiveMesh(const WorkloadConfig& config, std::uint64_t units)
      : descriptor_{std::string(kName), kWorkloadVersion, DeterminismClass::kBoundedResidual,
                    verification::Contract::kBounded, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_{kInputBytes, kOutputBytes, kScratchBytes, units, 1, kLogicalBytes, kOperations},
        seed_(config.seed) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  /// Hotspot h is hotspot_of(u64_of(block_at(seed, index, h))), written as
  /// its x then its y, each four little-endian bytes.
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t h = 0; h < kHotspots; ++h) {
      const Hotspot hotspot = hotspot_of(rng::u64_of(rng::block_at(seed_, index, h)));
      store_u32(input, h * kHotspotBytes, hotspot.x);
      store_u32(input, (h * kHotspotBytes) + sizeof(std::uint32_t), hotspot.y);
    }
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    Hotspots hotspots{};
    for (std::size_t h = 0; h < kHotspots; ++h) {
      hotspots[h] = Hotspot{load_u32(input, h * kHotspotBytes),
                            load_u32(input, (h * kHotspotBytes) + sizeof(std::uint32_t))};
    }
    std::array<Leaf, kMaxLeaves> leaves{};
    std::array<std::uint32_t, kCoarseCells> counts{};
    const std::size_t made = build_mesh(hotspots, leaves, counts);

    std::ranges::fill(scratch, std::byte{0});
    UnitWork work{leaves.data(), &hotspots, scratch.data()};
    executor.run(made, &run_leaf, &work);

    const std::size_t slots = executor.threads();
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      double integral = 0.0;
      for (std::size_t slot = 0; slot < slots; ++slot) {
        double held = 0.0;
        std::memcpy(&held, scratch.data() + (slot * kScratchBytes) + (cell * sizeof(double)),
                    sizeof held);
        integral += held;
      }
      store_u64(output, integral_offset(cell), std::bit_cast<std::uint64_t>(integral));
      store_u32(output, count_offset(cell), counts[cell]);
    }
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  if (config.decomposition > 1) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "decomposition " + std::to_string(config.decomposition) +
                             " is not 0 or 1: an adaptive mesh unit has no decomposition"};
  }
  if (config.isa != simd::IsaPath::kScalar) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "no adaptive_mesh kernel for ISA path " +
                             std::string(simd::describe(config.isa)) + "; this build has scalar"};
  }
  const std::uint64_t units = config.footprint_bytes / kUnitBytes;
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "a footprint of " + std::to_string(config.footprint_bytes) +
                             " bytes holds no unit of " + std::to_string(kUnitBytes) + " bytes"};
  }
  return std::unique_ptr<Workload>(std::make_unique<AdaptiveMesh>(config, units));
}

}  // namespace loadforge::workloads::adaptive_mesh
