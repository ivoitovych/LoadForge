// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/sparse/sparse_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace loadforge::verification::oracle::sparse {
namespace {

namespace spec = workloads::sparse;

/// The largest magnitude an off-diagonal value can have: 1/8.
constexpr double kMaxOffDiagonal = 0.125;

NotChecked not_checked(std::string what, std::string why) {
  return NotChecked{std::move(what), std::move(why), 0};
}

/// Whether the input is the fixed structure the tolerances depend on, with
/// values they cover; the reason it is not, or nothing. b is checked here too,
/// and its 2-norm returned through `b_norm`.
std::optional<NotChecked> input_problem(std::span<const std::byte> input, double& b_norm) {
  const std::span<const std::byte> values = input.subspan(spec::kValuesOffset);
  const std::span<const std::byte> rhs = input.subspan(spec::kRhsOffset);
  const std::span<const std::byte> columns = input.subspan(spec::kColumnsOffset);
  const std::span<const std::byte> row_ptr = input.subspan(spec::kRowPtrOffset);

  if (load_u32(row_ptr, 0) != 0) {
    return not_checked("sparse structure", "row_ptr does not start at zero");
  }
  std::size_t start = 0;
  for (std::size_t row = 0; row < spec::kRows; ++row) {
    const RowColumns expected = row_columns(row);
    const std::size_t next = load_u32(row_ptr, row + 1);
    if (next < start || next - start != expected.count) {
      return not_checked("sparse structure", "row_ptr does not give the fixed row lengths");
    }
    const std::span<const std::uint32_t> wanted(expected.columns.data(), expected.count);
    for (std::size_t entry = 0; entry < wanted.size(); ++entry) {
      if (load_u32(columns, start + entry) != wanted[entry]) {
        return not_checked("sparse structure", "a stored column is not the fixed structure's");
      }
      const double value = load_double(values, start + entry);
      if (wanted[entry] == row) {
        if (value != spec::kDiagonal) {
          return not_checked("sparse tolerance", "a diagonal entry is not exactly 2");
        }
      } else if (!(std::fabs(value) <= kMaxOffDiagonal)) {
        return not_checked("sparse tolerance",
                           "an off-diagonal value is not finite or has magnitude above 1/8");
      }
    }
    start = next;
  }

  double squares = 0.0;
  for (std::size_t row = 0; row < spec::kRows; ++row) {
    const double value = load_double(rhs, row);
    if (!(std::fabs(value) <= 1.0)) {
      return not_checked("sparse tolerance",
                         "a right-hand side entry is not finite or has magnitude above 1");
    }
    squares += value * value;
  }
  if (squares == 0.0) {
    return not_checked("sparse tolerance", "every right-hand side entry is zero, so tau is zero");
  }
  b_norm = std::sqrt(squares);
  return std::nullopt;
}

}  // namespace

double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept {
  std::uint64_t bits = 0;
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    bits |= std::to_integer<std::uint64_t>(bytes[element * sizeof(double) + byte]) << (8U * byte);
  }
  return std::bit_cast<double>(bits);
}

std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t element) noexcept {
  std::uint32_t word = 0;
  for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
    word |= std::to_integer<std::uint32_t>(bytes[element * sizeof(std::uint32_t) + byte])
            << (8U * byte);
  }
  return word;
}

RowColumns row_columns(std::size_t row) noexcept {
  RowColumns result;
  const std::span<std::uint32_t> slots(result.columns);
  for (const std::size_t offset : std::views::reverse(spec::kOffsets)) {
    if (row >= offset) {
      slots[result.count++] = static_cast<std::uint32_t>(row - offset);
    }
  }
  slots[result.count++] = static_cast<std::uint32_t>(row);
  for (const std::size_t offset : spec::kOffsets) {
    if (row + offset < spec::kRows) {
      slots[result.count++] = static_cast<std::uint32_t>(row + offset);
    }
  }
  return result;
}

double clamp_tolerance(double tolerance) noexcept {
  if (!(tolerance <= std::numeric_limits<double>::max())) {
    return std::numeric_limits<double>::max();
  }
  return std::max(tolerance, spec::kMinTolerance);
}

double row_tolerance(std::size_t entries, double abs_sum) noexcept {
  return clamp_tolerance(spec::row_tolerance_factor(entries) * abs_sum *
                         (1.0 + spec::kEvaluationSlack));
}

double residual_norm_tolerance(double b_norm) noexcept {
  return clamp_tolerance(spec::residual_norm_factor() * b_norm * (1.0 + spec::kEvaluationSlack));
}

std::string_view SparseOracle::workload() const noexcept { return spec::kName; }

Contract SparseOracle::contract() const noexcept { return Contract::kBounded; }

Verdict SparseOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    return not_checked("sparse unit",
                       "evidence is not 1065988 bytes of input and 262144 of output");
  }
  double b_norm = 0.0;
  if (std::optional<NotChecked> problem = input_problem(evidence.input, b_norm)) {
    return *std::move(problem);
  }

  const std::span<const std::byte> values = evidence.input.subspan(spec::kValuesOffset);
  const std::span<const std::byte> rhs = evidence.input.subspan(spec::kRhsOffset);
  const std::span<const std::byte> columns = evidence.input.subspan(spec::kColumnsOffset);
  const std::span<const std::byte> iterate = evidence.output.first(spec::kRows * sizeof(double));
  const std::span<const std::byte> residual_out =
      evidence.output.subspan(spec::kRows * sizeof(double));

  // r~ = b - A x^ row by row, with s~ = |b| + |A| |x^| beside it; the rows are
  // judged as they are formed, and their squares kept for the norm.
  double squares = 0.0;
  std::size_t start = 0;
  for (std::size_t row = 0; row < spec::kRows; ++row) {
    const std::size_t entries = spec::row_length(row);
    const double b = load_double(rhs, row);
    double residual = b;
    double abs_sum = std::fabs(b);
    for (std::size_t entry = start; entry < start + entries; ++entry) {
      const double a = load_double(values, entry);
      const double x = load_double(iterate, load_u32(columns, entry));
      residual -= a * x;
      abs_sum += std::fabs(a) * std::fabs(x);
    }
    start += entries;

    const double difference = std::fabs(load_double(residual_out, row) - residual);
    const double limit = row_tolerance(entries, abs_sum);
    if (!(difference <= limit)) {
      return BoundedMismatch{"residual-row", difference, limit, std::nullopt, row, row + 1};
    }
    squares += residual * residual;
  }

  const double norm = std::sqrt(squares);
  const double norm_limit = residual_norm_tolerance(b_norm);
  if (!(norm <= norm_limit)) {
    return BoundedMismatch{"residual-norm", norm,         norm_limit,
                           std::nullopt,    std::nullopt, spec::kRows + 1};
  }
  return Agreement{spec::kRows + 1};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& /*config*/, const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    std::string detail =
        "the plan's unit is not a 16384-row system in and its iterate and residual out";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  if (!spec::is_chunk_rows(plan.decomposition)) {
    std::string detail = "the plan's decomposition is not a chunk of 256, 512, 1024 or 2048 rows";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  std::unique_ptr<Oracle> oracle = std::make_unique<SparseOracle>();
  return oracle;
}

}  // namespace loadforge::verification::oracle::sparse
