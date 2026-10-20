// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the sparse oracle: scipy's conjugate gradients on a generated system
// are agreed with, a corrupted output is not, and the derived tolerances hold
// at the boundary the specification names -- breached at (1 + epsilon) times
// T' and held at (1 - epsilon) times it, for a row of 7 entries, a row of 4,
// and the residual norm.

#include "verification/oracle/sparse/sparse_oracle.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "oracle/verification/oracle/sparse/sparse_evidence.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::verification::oracle::sparse {
namespace {

namespace spec = workloads::sparse;
using evidence::System;
using evidence::Unit;

constexpr double kEpsilon = 1.0 / 1048576.0;  // 2^-20, the specification's

std::unique_ptr<Oracle> make_oracle(std::uint32_t chunk_rows = spec::kDefaultChunkRows) {
  auto created =
      create(workloads::WorkloadConfig{},
             workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, chunk_rows, 0, 0});
  return std::move(created).value();
}

contracts::OracleCase as_case(const Unit& unit) {
  return contracts::OracleCase{unit.index, unit.input, unit.output};
}

Verdict check_unit(const Oracle& oracle, const Unit& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

std::vector<contracts::OracleCase> real_tool_cases() {
  return {as_case(evidence::real_tool_unit())};
}

// Corruption of the iterate, of the residual and of both: a row check sees each
// wherever it is, because every row is compared.
std::vector<contracts::OracleCase> corrupted_cases() {
  const contracts::OracleCase base = as_case(evidence::real_tool_unit());
  std::vector<contracts::OracleCase> cases;
  for (const std::size_t element : {std::size_t{0}, std::size_t{9001}, 2 * spec::kRows - 1}) {
    contracts::OracleCase unit = base;
    unit.output[element * 8 + 7] ^= std::byte{0x40};
    cases.push_back(unit);
  }
  // The lowest exponent bit of an iterate entry: a factor of two in a value of
  // order one, far outside the bound (the residual entries are at the roundoff,
  // so only a flip of their top exponent bit is seen there).
  contracts::OracleCase exponent = base;
  exponent.output[777 * 8 + 6] ^= std::byte{0x10};
  cases.push_back(exponent);
  contracts::OracleCase nan = base;
  const std::vector<std::byte> nan_bytes = evidence::bytes_of(std::vector<double>(1, std::nan("")));
  std::copy(nan_bytes.begin(), nan_bytes.end(), nan.output.begin() + std::ptrdiff_t{4000} * 8);
  cases.push_back(nan);
  return cases;
}

double expected_row_limit(std::size_t entries, double abs_sum) {
  return spec::row_tolerance_factor(entries) * abs_sum * (1.0 + spec::kEvaluationSlack);
}

// A system with one off-diagonal pair, the entry (row, row + 1) and its mirror,
// of value 1/8, and a right-hand side of ones; and an iterate that nearly
// solves it, so that every residual is tiny and the norm check holds. The
// residual the oracle forms is mirrored here to the bit, so the offset made to
// the row's reported residual is the residual the oracle measures.
struct RowRig {
  System system;
  std::vector<double> x;
  evidence::Residual residual;

  explicit RowRig(std::size_t row)
      : system(evidence::build_system(
            [row](std::size_t i, std::size_t k) { return (i == row && k == 0) ? 0.125 : 0.0; },
            [](std::size_t) { return 1.0; })),
        x(spec::kRows, 0.5) {
    // [2 1/8; 1/8 2] [x_a; x_b] = [1; 1] has x_a = x_b = 1 / 2.125.
    const double solved = 1.0 / 2.125;
    x[row] = solved;
    x[row + 1] = solved;
    residual = evidence::explicit_residual(system, x);
  }

  [[nodiscard]] Unit unit_with_reported(std::size_t row, double reported) const {
    std::vector<double> reported_residual = residual.r;
    reported_residual[row] = reported;
    return Unit{0, evidence::input_bytes(system), evidence::output_bytes(x, reported_residual)};
  }
};

TEST(SparseOracleTolerance, ARowOffByMoreThanTheToleranceIsCaughtAndLessIsHeld) {
  const auto oracle = make_oracle();
  // Rows with 7, 5 and 4 stored entries, and the last pair at the far end.
  for (const std::size_t row :
       {std::size_t{5000}, std::size_t{50}, std::size_t{0}, spec::kRows - 2}) {
    const RowRig rig(row);
    // The row whose pair was set has all its entries; its length decides the factor.
    for (const std::size_t target : {row, row + 1}) {
      const std::size_t entries = spec::row_length(target);
      const double limit = expected_row_limit(entries, rig.residual.s[target]);
      ASSERT_GT(rig.residual.s[target], 1.5) << "the off-diagonal term is part of the sum";
      ASSERT_LT(std::fabs(rig.residual.r[target]), 1e-15);
      for (const double sign : {1.0, -1.0}) {
        const double base = rig.residual.r[target];
        const Verdict breached = check_unit(
            *oracle, rig.unit_with_reported(target, base + sign * limit * (1.0 + kEpsilon)));
        ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(breached))
            << "row " << target << " (" << entries << " entries)";
        const auto& mismatch = std::get<BoundedMismatch>(breached);
        EXPECT_EQ(mismatch.check, "residual-row");
        EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>(target));
        EXPECT_EQ(mismatch.blocks_compared, target + 1);
        EXPECT_EQ(mismatch.tolerance, limit);
        EXPECT_NEAR(mismatch.residual, limit * (1.0 + kEpsilon), limit * 1e-9);
        EXPECT_TRUE(is_valid(breached, Contract::kBounded));

        const Verdict held = check_unit(
            *oracle, rig.unit_with_reported(target, base + sign * limit * (1.0 - kEpsilon)));
        ASSERT_TRUE(std::holds_alternative<Agreement>(held))
            << "row " << target << " (" << entries << " entries)";
        EXPECT_EQ(std::get<Agreement>(held).blocks_compared, spec::kRows + 1);
      }
    }
  }
}

TEST(SparseOracleTolerance, TheResidualNormIsBreachedJustAboveItsBoundAndHeldJustBelow) {
  const auto oracle = make_oracle();
  // A = 2 I (every off-diagonal value zero), b = e_0, x = e_0 / 2: the residual
  // is exactly zero. Moving x_5 to eta makes it -2 eta at row 5 alone, whose
  // norm is exactly 2 eta, and the reported residual follows it.
  const System system =
      evidence::uniform_system(0.0, [](std::size_t i) { return i == 0 ? 1.0 : 0.0; });
  const double bound = 3.0 * std::pow(5.0, -16.0) * 1.0 * (1.0 + spec::kEvaluationSlack);
  for (const double scale : {1.0 + kEpsilon, 1.0 - kEpsilon}) {
    const double residual_norm = bound * scale;
    std::vector<double> x(spec::kRows, 0.0);
    x[0] = 0.5;
    x[5] = residual_norm / 2.0;
    std::vector<double> reported(spec::kRows, 0.0);
    reported[5] = -residual_norm;
    const Unit unit{0, evidence::input_bytes(system), evidence::output_bytes(x, reported)};
    const Verdict verdict = check_unit(*oracle, unit);
    if (scale > 1.0) {
      ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
      const auto& mismatch = std::get<BoundedMismatch>(verdict);
      EXPECT_EQ(mismatch.check, "residual-norm");
      EXPECT_EQ(mismatch.element, std::nullopt);
      EXPECT_EQ(mismatch.blocks_compared, spec::kRows + 1);
      EXPECT_EQ(mismatch.tolerance, bound);
      EXPECT_EQ(mismatch.residual, residual_norm);
      EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
    } else {
      ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
      EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kRows + 1);
    }
  }
}

TEST(SparseOracleRealTool, ScipysConjugateGradientsAreAgreedWithAtEveryChunkSize) {
  for (const std::uint32_t chunk : spec::kChunkRows) {
    const auto oracle = make_oracle(chunk);
    const Verdict verdict = check_unit(*oracle, evidence::real_tool_unit());
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "chunk " << chunk;
    EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kRows + 1);
  }
}

TEST(SparseOracleRealTool, ScipysResidualIsFarInsideTheNormBound) {
  // The specification's own statement that the bound is held by a factor of
  // about 10^5: the fixture's ||r|| / ||b|| is at the roundoff, not near 2e-11.
  const Unit unit = evidence::real_tool_unit();
  const std::vector<double> output = evidence::doubles_of(unit.output);
  const System system = evidence::generated_system(evidence::kSeed, unit.index);
  double norm_r = 0.0;
  double norm_b = 0.0;
  for (std::size_t i = 0; i < spec::kRows; ++i) {
    norm_r += output[spec::kRows + i] * output[spec::kRows + i];
    norm_b += system.rhs[i] * system.rhs[i];
  }
  EXPECT_LT(std::sqrt(norm_r) / std::sqrt(norm_b), 1e-14);
  EXPECT_GT(spec::residual_norm_factor() / (std::sqrt(norm_r) / std::sqrt(norm_b)), 1e4);
}

}  // namespace
}  // namespace loadforge::verification::oracle::sparse

// The suites are instantiated where they are declared: the macros name them
// unqualified.
namespace loadforge::contracts {
namespace {

namespace under_test = verification::oracle::sparse;

// This binary instantiates the oracle suite only.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);

struct SparseSubject {
  static constexpr const char* kWorkload = "sparse";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Sparse, OracleContract, ::testing::Types<SparseSubject>);

}  // namespace
}  // namespace loadforge::contracts
