// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_SPARSE_SPARSE_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_SPARSE_SPARSE_ORACLE_HPP

// The independent oracle of the sparse workload (verification.md §3, §5),
// written from src/workloads/sparse/spec.hpp and from nothing else: it forms
// the explicit residual r~ = b - A x^ of the output's iterate by a plain scalar
// sparse matrix-vector product, compares every row of it with the residual the
// workload wrote, and checks the residual's norm against the conjugate-gradient
// bound. Contract::kBounded.
//
// It allocates nothing: the matrix, b and the output are read in place, one row
// at a time, so check() cannot fail to allocate scratch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::sparse {

/// The columns of a row's stored entries, in increasing order, as the fixed
/// structure of the specification places them; `count` of the seven are used.
struct RowColumns {
  std::array<std::uint32_t, 7> columns{};
  std::size_t count = 0;
};

/// Precondition: row < kRows.
[[nodiscard]] RowColumns row_columns(std::size_t row) noexcept;

/// Limits a tolerance to [kMinTolerance, the largest finite double]; a NaN or
/// an infinity is the largest. The result is always finite and positive.
[[nodiscard]] double clamp_tolerance(double tolerance) noexcept;

/// T'_i for a row of `entries` stored entries whose s~_i is `abs_sum`:
/// clamp(row_tolerance_factor(entries) * abs_sum * (1 + kEvaluationSlack)).
[[nodiscard]] double row_tolerance(std::size_t entries, double abs_sum) noexcept;

/// tau' for a right-hand side of 2-norm `b_norm`:
/// clamp(residual_norm_factor() * b_norm * (1 + kEvaluationSlack)).
[[nodiscard]] double residual_norm_tolerance(double b_norm) noexcept;

/// The little-endian double at `element` of a span of doubles stored as bytes.
/// Precondition: (element + 1) * 8 <= bytes.size().
[[nodiscard]] double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept;

/// The little-endian u32 at `element` of a span of u32 stored as bytes.
/// Precondition: (element + 1) * 4 <= bytes.size().
[[nodiscard]] std::uint32_t load_u32(std::span<const std::byte> bytes,
                                     std::size_t element) noexcept;

class SparseOracle final : public Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;
};

/// The oracle for a plan the sparse workload resolved: kConfiguration when the
/// plan's sizes are not a unit of the specification or its decomposition is not
/// one of its chunk sizes. (The chunk size does not change what is checked; the
/// plan is still held to the specification.)
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::sparse

#endif
