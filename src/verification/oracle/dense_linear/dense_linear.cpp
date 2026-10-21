// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/dense_linear/dense_linear.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "verification/residual/residual.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::dense_linear {
namespace {

namespace spec = workloads::dense_linear;

constexpr std::size_t kN = spec::kOrder;
constexpr std::size_t kMatrixEntries = kN * kN;
constexpr std::string_view kWhat = "dense_linear unit";
constexpr std::string_view kCheck = "componentwise |C - AB| on sampled entries";

// The specification's doubles are little-endian, and so is every machine this
// builds for (x86-64, ARM64): a copy of the bytes is the value. A big-endian
// target would need a byte swap here, and fails to compile until it has one.
static_assert(std::endian::native == std::endian::little);

/// Element `element` of a span of little-endian doubles.
double load(std::span<const std::byte> bytes, std::size_t element) noexcept {
  double value = 0.0;
  std::memcpy(&value, bytes.subspan(element * sizeof(double), sizeof(double)).data(),
              sizeof(double));
  return value;
}

/// The shortest text that reads back as `value`: what a report shows of a
/// corrupt entry, which must be the entry and not a rounded neighbour.
std::string text_of(double value) {
  std::array<char, 32> buffer{};
  const std::to_chars_result written = std::to_chars(buffer.begin(), buffer.end(), value);
  return {buffer.begin(), written.ptr};
}

/// "A[i][j]" or "B[i][j]" for input element `element`.
std::string name_input(std::size_t element) {
  const std::size_t within = element % kMatrixEntries;
  std::string name = element < kMatrixEntries ? "A[" : "B[";
  name += std::to_string(within / kN);
  name += "][";
  name += std::to_string(within % kN);
  name += ']';
  return name;
}

/// Whether `candidate` is a worse failure than `current`: a NaN residual
/// before any number, then the greater ratio; never on a tie, so the first
/// in tile order is kept.
bool worse(const residual::Comparison& candidate, const residual::Comparison& current) noexcept {
  if (std::isnan(current.residual)) {
    return false;
  }
  if (std::isnan(candidate.residual)) {
    return true;
  }
  return candidate.residual / candidate.tolerance > current.residual / current.tolerance;
}

class DenseLinearOracle final : public Oracle {
 public:
  explicit DenseLinearOracle(std::size_t grid) noexcept : grid_(grid) {}

  [[nodiscard]] std::string_view workload() const noexcept override { return kWorkload; }
  [[nodiscard]] Contract contract() const noexcept override { return Contract::kBounded; }

  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override {
    // Each text is built in a statement of its own before the verdict, so no
    // verdict is ever half built when an allocation could throw: the cleanup
    // block such a throw would need carries a branch no test can take
    // (journal §1.13).
    if (evidence.input.size() != spec::kInputBytes) {
      std::string why = "the input is " + std::to_string(evidence.input.size());
      why += " bytes, not " + std::to_string(spec::kInputBytes);
      return NotChecked{std::string(kWhat), std::move(why), 0};
    }
    if (evidence.output.size() != spec::kOutputBytes) {
      std::string why = "the output is " + std::to_string(evidence.output.size());
      why += " bytes, not " + std::to_string(spec::kOutputBytes);
      return NotChecked{std::string(kWhat), std::move(why), 0};
    }
    for (std::size_t element = 0; element < 2 * kMatrixEntries; ++element) {
      const double value = load(evidence.input, element);
      // The generator's range, [-1, 1). Named rather than negated in place:
      // the De Morgan form, value < -1 || value >= 1, would let a NaN through.
      const bool in_range = value >= -1.0 && value < 1.0;
      if (!in_range) {
        std::string why = "input " + name_input(element);
        why += " is " + text_of(value);
        why += ", outside [-1, 1): the tolerance cannot be derived";
        return NotChecked{std::string(kWhat), std::move(why), 0};
      }
    }
    return compare_sample(evidence);
  }

 private:
  [[nodiscard]] Verdict compare_sample(const UnitEvidence& evidence) const {
    const std::size_t tiles = grid_ * grid_;
    std::optional<residual::Comparison> worst;
    Entry worst_entry;
    double worst_reference = 0.0;
    double worst_magnitude = 0.0;
    std::array<double, kN> row{};
    std::array<double, kN> column{};
    for (std::size_t tile = 0; tile < tiles; ++tile) {
      const Entry entry = sample_entry(evidence.index, grid_, tile);
      for (std::size_t k = 0; k < kN; ++k) {
        row.at(k) = load(evidence.input, entry.row * kN + k);
        column.at(k) = load(evidence.input, kMatrixEntries + k * kN + entry.column);
      }
      const residual::Accumulation reference = residual::dot(row, column);
      // In range by the scan above, so the magnitude is at most n and the
      // tolerance is always derived: value() cannot meet the error arm.
      const double tolerance =
          residual::tolerance(residual::dot_model(kN), reference.magnitude).value();
      const residual::Comparison comparison = residual::compare(
          load(evidence.output, entry.row * kN + entry.column), reference.value, tolerance);
      if (!residual::within(comparison) && (!worst.has_value() || worse(comparison, *worst))) {
        worst = comparison;
        worst_entry = entry;
        worst_reference = reference.value;
        worst_magnitude = reference.magnitude;
      }
    }
    if (!worst.has_value()) {
      return Agreement{tiles};
    }
    std::optional<double> condition;
    if (worst_reference != 0.0) {
      condition = worst_magnitude / std::abs(worst_reference);
    }
    return BoundedMismatch{std::string(kCheck),
                           worst->residual,
                           worst->tolerance,
                           condition,
                           worst_entry.row * kN + worst_entry.column,
                           tiles};
  }

  std::size_t grid_;
};

}  // namespace

bool is_grid(std::size_t grid) noexcept { return grid <= kN && std::has_single_bit(grid); }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named in the header's order.
Entry sample_entry(std::uint64_t index, std::size_t grid, std::size_t tile) noexcept {
  const std::size_t edge = kN / grid;
  // std::size_t is std::uint64_t on every platform this builds for (LP64).
  const std::size_t bits = rng::u64_of(rng::block_at(kSampleSeed, index, tile));
  const std::size_t mask = edge - 1;
  return Entry{(tile / grid) * edge + (bits & mask), (tile % grid) * edge + ((bits >> 32U) & mask)};
}

core::Result<std::unique_ptr<Oracle>, OracleError> make_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan, std::size_t grid) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    return OracleError{OracleFault::kConfiguration,
                       "the plan's unit is " + std::to_string(plan.input_bytes) + " bytes in and " +
                           std::to_string(plan.output_bytes) + " out; a dense_linear unit is " +
                           std::to_string(spec::kInputBytes) + " and " +
                           std::to_string(spec::kOutputBytes)};
  }
  if (!spec::is_panel_depth(plan.decomposition)) {
    return OracleError{
        OracleFault::kConfiguration,
        "panel depth " + std::to_string(plan.decomposition) + " is not one the specification has"};
  }
  if (config.decomposition != 0 && config.decomposition != plan.decomposition) {
    return OracleError{OracleFault::kConfiguration, "the configuration asks for panel depth " +
                                                        std::to_string(config.decomposition) +
                                                        " and the plan resolved " +
                                                        std::to_string(plan.decomposition)};
  }
  if (!is_grid(grid)) {
    return OracleError{OracleFault::kConfiguration,
                       "a sample grid of " + std::to_string(grid) +
                           " does not divide the output into whole tiles"};
  }
  return std::unique_ptr<Oracle>(std::make_unique<DenseLinearOracle>(grid));
}

}  // namespace loadforge::verification::oracle::dense_linear
