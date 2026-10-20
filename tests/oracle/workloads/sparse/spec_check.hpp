// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_SPARSE_SPEC_CHECK_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_SPARSE_SPEC_CHECK_HPP

// The sparse check, written from spec.hpp's "THE CHECK, AND ITS BOUNDS" and
// from nothing in src/workloads/sparse/ but the specification header itself:
// a scalar SpMV r~ = b - A x^ with std::fma in ascending order, n row checks,
// then the residual norm. It is the sparse tests' own reading of what the
// independent oracle does (the oracle stream writes the real one), and the
// tiers that need a verdict -- the oracle tier, fault injection, determinism
// and the properties -- all use this one. It reads the matrix from the
// bytes it is given and builds the structure it expects from kOffsets, never
// from the workload.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse::check {

enum class Verdict { kAgreement, kBoundedMismatch, kNotChecked };

struct Outcome {
  Verdict verdict = Verdict::kNotChecked;
  std::size_t blocks_compared = 0;
  std::string check;  ///< "residual-row" or "residual-norm", for a mismatch
  std::size_t element = 0;
  double residual = 0.0;
  double tolerance = 0.0;
  std::string why;  ///< for NotChecked
};

inline double f64_at(std::span<const std::byte> bytes, std::size_t offset, std::size_t index) {
  double value = 0.0;
  std::memcpy(&value, bytes.data() + offset + index * sizeof(double), sizeof(double));
  return value;
}

inline std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t offset,
                            std::size_t index) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset + index * sizeof(std::uint32_t),
              sizeof(std::uint32_t));
  return value;
}

inline double clamp_tolerance(double evaluated) {
  const double slackened = evaluated * (1.0 + kEvaluationSlack);
  return std::clamp(slackened, kMinTolerance, std::numeric_limits<double>::max());
}

inline Outcome not_checked(std::string why) {
  Outcome outcome;
  outcome.why = std::move(why);
  return outcome;
}

/// The specification's check of `output` against `input`.
inline Outcome check(std::span<const std::byte> input, std::span<const std::byte> output) {
  if (input.size() != kInputBytes || output.size() != kOutputBytes) {
    return not_checked("evidence of the wrong size");
  }
  // The input must be what the specification generates, in the ways the
  // tolerances depend on.
  std::size_t entry = 0;
  for (std::size_t row = 0; row < kRows; ++row) {
    if (u32_at(input, kRowPtrOffset, row) != entry) {
      return not_checked("row_ptr differs from the fixed structure");
    }
    std::vector<std::size_t> expected;
    for (std::size_t k = kOffsets.size(); k-- > 0;) {
      if (row >= kOffsets[k]) {
        expected.push_back(row - kOffsets[k]);
      }
    }
    expected.push_back(row);
    for (const std::size_t offset : kOffsets) {
      if (row + offset < kRows) {
        expected.push_back(row + offset);
      }
    }
    for (const std::size_t column : expected) {
      if (u32_at(input, kColumnsOffset, entry) != column) {
        return not_checked("columns differ from the fixed structure");
      }
      const double value = f64_at(input, kValuesOffset, entry);
      if (!std::isfinite(value)) {
        return not_checked("a value is not finite");
      }
      if (column == row ? value != kDiagonal : std::fabs(value) > 0.125) {
        return not_checked("a diagonal entry is not 2.0 or an off-diagonal one exceeds 1/8");
      }
      ++entry;
    }
  }
  if (u32_at(input, kRowPtrOffset, kRows) != entry || entry != kNonZeros) {
    return not_checked("row_ptr differs from the fixed structure");
  }
  double b_norm_squared = 0.0;
  bool any_nonzero = false;
  for (std::size_t row = 0; row < kRows; ++row) {
    const double b = f64_at(input, kRhsOffset, row);
    if (!std::isfinite(b) || std::fabs(b) > 1.0) {
      return not_checked("a right-hand side entry is not finite or exceeds 1");
    }
    any_nonzero = any_nonzero || b != 0.0;
    b_norm_squared += b * b;
  }
  if (!any_nonzero) {
    return not_checked("the right-hand side is zero");
  }

  // r~ = b - A x^, and s~_i = |b_i| + sum |a_ij| |x^_j|.
  Outcome outcome;
  std::vector<double> residual(kRows);
  entry = 0;
  for (std::size_t row = 0; row < kRows; ++row) {
    const std::size_t last = u32_at(input, kRowPtrOffset, row + 1);
    double signed_sum = f64_at(input, kRhsOffset, row);
    double scale = std::fabs(signed_sum);
    for (; entry < last; ++entry) {
      const std::size_t column = u32_at(input, kColumnsOffset, entry);
      const double a = f64_at(input, kValuesOffset, entry);
      const double x = f64_at(output, 0, column);
      signed_sum = std::fma(-a, x, signed_sum);
      scale += std::fabs(a) * std::fabs(x);
    }
    residual[row] = signed_sum;
    const double tolerance =
        clamp_tolerance(row_tolerance_factor(row_length(row)) * scale);
    const double difference = std::fabs(f64_at(output, kRows * sizeof(double), row) - signed_sum);
    ++outcome.blocks_compared;
    if (!(difference <= tolerance)) {
      outcome.verdict = Verdict::kBoundedMismatch;
      outcome.check = "residual-row";
      outcome.element = row;
      outcome.residual = difference;
      outcome.tolerance = tolerance;
      return outcome;
    }
  }
  double squares = 0.0;
  for (const double r : residual) {
    squares += r * r;
  }
  const double norm = std::sqrt(squares);
  const double tau = clamp_tolerance(residual_norm_factor() * std::sqrt(b_norm_squared));
  ++outcome.blocks_compared;
  outcome.residual = norm;
  outcome.tolerance = tau;
  if (!(norm <= tau)) {
    outcome.verdict = Verdict::kBoundedMismatch;
    outcome.check = "residual-norm";
    return outcome;
  }
  outcome.verdict = Verdict::kAgreement;
  return outcome;
}

}  // namespace loadforge::workloads::sparse::check

#endif
