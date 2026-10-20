// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/stencil/stencil_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace loadforge::verification::oracle::stencil {
namespace {

namespace spec = workloads::stencil;

constexpr std::size_t kSpan = spec::kEdge - spec::kRegionEdge + 1;
/// A window grown by kSteps on each side, and a ring of zero padding around it.
constexpr std::size_t kStride = spec::kRegionEdge + 2 * spec::kSteps + 2;
constexpr std::size_t kBuffer = kStride * kStride;

constexpr std::uint64_t kSaltRowTwo = 0x1F83D9ABFB41BD6BULL;
constexpr std::uint64_t kSaltColumnTwo = 0x5BE0CD19137E2179ULL;
constexpr std::uint64_t kSaltRowThree = 0x6A09E667F3BCC908ULL;
constexpr std::uint64_t kSaltColumnThree = 0xBB67AE8584CAA73BULL;

/// The splitmix64 finaliser: a bijective mixing of 64 bits, unsigned throughout.
constexpr std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

NotChecked not_checked(std::string what, std::string why) {
  return NotChecked{std::move(what), std::move(why), 0};
}

}  // namespace

double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept {
  std::uint64_t bits = 0;
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    bits |= std::to_integer<std::uint64_t>(bytes[element * sizeof(double) + byte]) << (8U * byte);
  }
  return std::bit_cast<double>(bits);
}

std::array<Origin, spec::kRegions> window_origins(std::uint64_t index) noexcept {
  return {Origin{0, 0}, Origin{spec::kEdge - spec::kRegionEdge, spec::kEdge - spec::kRegionEdge},
          Origin{mix(index ^ kSaltRowTwo) % kSpan, mix(index ^ kSaltColumnTwo) % kSpan},
          Origin{mix(index ^ kSaltRowThree) % kSpan, mix(index ^ kSaltColumnThree) % kSpan}};
}

Window expected_window(std::span<const std::byte> input, Origin origin) noexcept {
  const std::size_t first_row = origin.row >= spec::kSteps ? origin.row - spec::kSteps : 0;
  const std::size_t first_column = origin.column >= spec::kSteps ? origin.column - spec::kSteps : 0;
  const std::size_t end_row = std::min(origin.row + spec::kRegionEdge + spec::kSteps, spec::kEdge);
  const std::size_t end_column =
      std::min(origin.column + spec::kRegionEdge + spec::kSteps, spec::kEdge);
  const std::size_t rows = end_row - first_row;
  const std::size_t columns = end_column - first_column;

  // Both buffers start zero and only the interior of the grown window is ever
  // written, so the padding stays zero. A cell the window cuts off from its
  // true neighbours reads that zero, and the error so made travels one cell
  // per sweep, which is exactly the margin the window was grown by.
  std::array<double, kBuffer> first{};
  std::array<double, kBuffer> second{};
  std::span<double> current(first);
  std::span<double> next(second);
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < columns; ++c) {
      current[(r + 1) * kStride + c + 1] =
          load_double(input, (first_row + r) * spec::kEdge + first_column + c);
    }
  }
  for (std::size_t step = 0; step < spec::kSteps; ++step) {
    for (std::size_t r = 0; r < rows; ++r) {
      for (std::size_t c = 0; c < columns; ++c) {
        const std::size_t at = (r + 1) * kStride + c + 1;
        if (spec::is_ring(first_row + r, first_column + c)) {
          next[at] = current[at];
          continue;
        }
        double value = 0.5 * current[at];
        value += 0.125 * current[at - kStride];
        value += 0.125 * current[at + kStride];
        value += 0.125 * current[at - 1];
        value += 0.125 * current[at + 1];
        next[at] = value;
      }
    }
    std::swap(current, next);
  }

  Window window{};
  for (std::size_t r = 0; r < spec::kRegionEdge; ++r) {
    for (std::size_t c = 0; c < spec::kRegionEdge; ++c) {
      window[r * spec::kRegionEdge + c] = current[(origin.row - first_row + r + 1) * kStride +
                                                  (origin.column - first_column + c + 1)];
    }
  }
  return window;
}

double cell_tolerance(double max_magnitude) noexcept {
  return spec::cell_tolerance_factor() * max_magnitude * (1.0 + spec::kEvaluationSlack);
}

std::string_view StencilOracle::workload() const noexcept { return spec::kName; }

Contract StencilOracle::contract() const noexcept { return Contract::kBounded; }

Verdict StencilOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    return not_checked("stencil unit",
                       "evidence is not 524288 bytes of input and 524288 of output");
  }

  double max_magnitude = 0.0;
  for (std::size_t cell = 0; cell < spec::kEdge * spec::kEdge; ++cell) {
    const double magnitude = std::fabs(load_double(evidence.input, cell));
    if (!(magnitude <= 1.0)) {
      return not_checked("stencil tolerance",
                         "an input cell is not finite or has magnitude above 1, so the "
                         "tolerance cannot be derived");
    }
    max_magnitude = std::max(max_magnitude, magnitude);
  }
  if (max_magnitude == 0.0) {
    return not_checked("stencil tolerance", "every input cell is zero, so the tolerance is zero");
  }
  const double limit = cell_tolerance(max_magnitude);

  std::uint64_t compared = 0;
  for (const Origin origin : window_origins(evidence.index)) {
    const Window expected = expected_window(evidence.input, origin);
    for (std::size_t r = 0; r < spec::kRegionEdge; ++r) {
      for (std::size_t c = 0; c < spec::kRegionEdge; ++c) {
        ++compared;
        const std::size_t element = (origin.row + r) * spec::kEdge + origin.column + c;
        const double actual = load_double(evidence.output, element);
        const double residual = std::fabs(actual - expected[r * spec::kRegionEdge + c]);
        if (!(residual <= limit)) {
          return BoundedMismatch{"stencil-cell", residual, limit, std::nullopt, element, compared};
        }
      }
    }
  }
  return Agreement{compared};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& /*config*/, const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    std::string detail = "the plan's unit is not a 256 x 256 grid of doubles in and out";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  if (!spec::is_tile_edge(plan.decomposition)) {
    std::string detail = "the plan's decomposition is not a tile edge of 16, 32, 64 or 128";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  std::unique_ptr<Oracle> oracle = std::make_unique<StencilOracle>();
  return oracle;
}

}  // namespace loadforge::verification::oracle::stencil
