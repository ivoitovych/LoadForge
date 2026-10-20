// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/adaptive_mesh/oracle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::adaptive_mesh {
namespace {

namespace spec = workloads::adaptive_mesh;

constexpr std::string_view kWhat = "the integrals and leaf counts against a recursively built mesh";
constexpr std::size_t kSampleWidth = spec::kCoarseCells / spec::kSampledCells;

using Hotspots = std::vector<spec::Hotspot>;

std::uint64_t load_le64(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint64_t value = 0;
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    value |= std::to_integer<std::uint64_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

std::uint32_t load_le32(std::span<const std::byte> bytes, std::size_t at) noexcept {
  std::uint32_t value = 0;
  for (std::size_t k = 0; k < sizeof(value); ++k) {
    value |= std::to_integer<std::uint32_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

/// The hotspots that lie inside a cell, in index order: the only ones that can
/// make a cell under it split.
using Inside = std::vector<spec::Hotspot>;

Inside hotspots_in(const spec::Cell& cell, const Hotspots& all) {
  Inside inside;
  for (const spec::Hotspot& hotspot : all) {
    if (spec::cell_contains(cell, hotspot)) {
      inside.push_back(hotspot);
    }
  }
  return inside;
}

bool splits(const spec::Cell& cell, const Inside& inside) noexcept {
  return cell.level < spec::kMaxLevel &&
         std::any_of(inside.begin(), inside.end(), [&cell](const spec::Hotspot& hotspot) {
           return spec::cell_contains(cell, hotspot);
         });
}

/// The leaves under `cell`, depth first, the children of a split cell in the
/// order q = 0, 1, 2, 3; `visit` is called with each.
// The recursion is bounded by kMaxLevel - kBaseLevel = 5 levels, and it is the
// specification's own description of the leaf order.
template <typename Visit>
// NOLINTNEXTLINE(misc-no-recursion)
void for_each_leaf(const spec::Cell& cell, const Inside& inside, Visit& visit) {
  if (!splits(cell, inside)) {
    visit(cell);
    return;
  }
  for (std::uint32_t q = 0; q < 4; ++q) {
    for_each_leaf(spec::Cell{cell.level + 1, 2 * cell.ix + (q & 1U), 2 * cell.iy + (q >> 1U)},
                  inside, visit);
  }
}

spec::Cell coarse_cell(std::size_t c) noexcept {
  return spec::Cell{spec::kBaseLevel, static_cast<std::uint32_t>(c % spec::kBaseCells),
                    static_cast<std::uint32_t>(c / spec::kBaseCells)};
}

std::uint32_t leaf_count(std::size_t c, const Hotspots& all) {
  const spec::Cell cell = coarse_cell(c);
  const Inside inside = hotspots_in(cell, all);
  std::uint32_t leaves = 0;
  auto count = [&leaves](const spec::Cell& /*leaf*/) noexcept { ++leaves; };
  for_each_leaf(cell, inside, count);
  return leaves;
}

/// c': the leaves of the coarse cell added in leaf order, one by one, from +0.0.
double integral(std::size_t c, const Hotspots& all) {
  const spec::Cell cell = coarse_cell(c);
  const Inside inside = hotspots_in(cell, all);
  double total = 0.0;
  auto add = [&total, &all](const spec::Cell& leaf) noexcept {
    const spec::Point point{spec::cell_centre(leaf.level, leaf.ix),
                            spec::cell_centre(leaf.level, leaf.iy)};
    double f = 0.0;
    for (const spec::Hotspot& hotspot : all) {
      f = f + spec::hotspot_term(point, hotspot);
    }
    const double x_leaf = f * spec::leaf_area(leaf.level);
    total = total + x_leaf;
  };
  for_each_leaf(cell, inside, add);
  return total;
}

/// splitmix64's finaliser: a fixed, well-mixed function of one word.
std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::string numbered(std::string_view check, std::size_t number) {
  std::string text(check);
  text += ' ';
  text += std::to_string(number);
  return text;
}

OracleError refused(std::string detail) {
  return OracleError{OracleFault::kConfiguration, std::move(detail)};
}

}  // namespace

std::size_t sampled_cell(std::uint64_t index, std::size_t j) noexcept {
  const std::uint64_t within = mix(mix(index) + j) % kSampleWidth;
  return j * kSampleWidth + within;
}

AdaptiveMeshOracle::AdaptiveMeshOracle() noexcept = default;

std::string_view AdaptiveMeshOracle::workload() const noexcept { return kWorkload; }

Contract AdaptiveMeshOracle::contract() const noexcept { return Contract::kBounded; }

Verdict AdaptiveMeshOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    std::string why = "the evidence is ";
    why += std::to_string(evidence.input.size());
    why += " bytes of input and ";
    why += std::to_string(evidence.output.size());
    why += " of output; an adaptive mesh unit is ";
    why += std::to_string(spec::kInputBytes);
    why += " and ";
    why += std::to_string(spec::kOutputBytes);
    return NotChecked{std::string(kWhat), std::move(why), 0};
  }
  Hotspots hotspots(spec::kHotspots);
  for (std::size_t h = 0; h < spec::kHotspots; ++h) {
    hotspots[h] = spec::Hotspot{load_le32(evidence.input, h * spec::kHotspotBytes),
                                load_le32(evidence.input, h * spec::kHotspotBytes + 4)};
    if (hotspots[h].x >= spec::kLattice || hotspots[h].y >= spec::kLattice) {
      std::string why = "hotspot ";
      why += std::to_string(h);
      why += " is at (";
      why += std::to_string(hotspots[h].x);
      why += ", ";
      why += std::to_string(hotspots[h].y);
      why += "), not below ";
      why += std::to_string(spec::kLattice);
      return NotChecked{std::string(kWhat), std::move(why), 0};
    }
  }
  const std::span<const std::byte> output = evidence.output;
  std::vector<std::uint32_t> leaves(spec::kCoarseCells);
  std::uint64_t compared = 0;
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    ++compared;
    leaves[c] = leaf_count(c, hotspots);
    const std::uint32_t stored = load_le32(output, spec::count_offset(c));
    if (stored != leaves[c]) {
      const double residual =
          std::fabs(static_cast<double>(stored) - static_cast<double>(leaves[c]));
      return BoundedMismatch{numbered(spec::kCheckLeafCount, c),
                             residual,
                             spec::kCountTolerance,
                             std::nullopt,
                             c,
                             compared};
    }
  }
  for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
    ++compared;
    const std::size_t c = sampled_cell(evidence.index, j);
    const double reference = integral(c, hotspots);
    const double tolerance = spec::cell_tolerance(leaves[c], reference);
    const auto stored = std::bit_cast<double>(load_le64(output, spec::integral_offset(c)));
    const double residual = std::fabs(stored - reference);
    if (!(residual <= tolerance)) {
      return BoundedMismatch{
          numbered(spec::kCheckIntegral, c), residual, tolerance, std::nullopt, c, compared};
    }
  }
  return Agreement{compared};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(const workloads::WorkloadConfig& config,
                                                          const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    return refused("an adaptive mesh unit is " + std::to_string(spec::kInputBytes) +
                   " bytes in and " + std::to_string(spec::kOutputBytes) + " out; the plan says " +
                   std::to_string(plan.input_bytes) + " and " + std::to_string(plan.output_bytes));
  }
  if (plan.decomposition != 1 || config.decomposition > 1) {
    return refused(
        "the adaptive mesh workload has no decomposition: the plan's must be 1 and the "
        "configured one 0 or 1, not " +
        std::to_string(plan.decomposition) + " and " + std::to_string(config.decomposition));
  }
  return std::unique_ptr<Oracle>(std::make_unique<AdaptiveMeshOracle>());
}

}  // namespace loadforge::verification::oracle::adaptive_mesh
