// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the sparse oracle: its structure, its tolerance functions and the
// plans it refuses, each on its own.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "oracle/verification/oracle/sparse/sparse_evidence.hpp"
#include "verification/oracle/sparse/sparse_oracle.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::verification::oracle::sparse {
namespace {

namespace spec = workloads::sparse;

TEST(SparseLoad, ReadsLittleEndianWhateverTheHostIs) {
  const std::vector<std::byte> bytes = {
      std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},   std::byte{0}, std::byte{0},
      std::byte{0xF0}, std::byte{0x3F}, std::byte{1},    std::byte{2},   std::byte{3}, std::byte{4},
      std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
  EXPECT_EQ(load_double(bytes, 0), 1.0);
  EXPECT_EQ(load_u32(bytes, 2), 0x04030201U);
  EXPECT_EQ(load_u32(bytes, 3), 0xFFFFFFFFU);
  EXPECT_EQ(load_u32(bytes, 0), 0U);
}

TEST(SparseRowColumns, AreTheFixedStructureForEveryRow) {
  std::size_t total = 0;
  for (std::size_t row = 0; row < spec::kRows; ++row) {
    const RowColumns found = row_columns(row);
    ASSERT_EQ(found.count, spec::row_length(row)) << "row " << row;
    ASSERT_GE(found.count, 4U);
    ASSERT_LE(found.count, 7U);
    bool has_diagonal = false;
    for (std::size_t entry = 0; entry < found.count; ++entry) {
      ASSERT_LT(found.columns.at(entry), spec::kRows) << "row " << row;
      has_diagonal = has_diagonal || found.columns.at(entry) == row;
      if (entry > 0) {
        ASSERT_LT(found.columns.at(entry - 1), found.columns.at(entry)) << "row " << row;
      }
    }
    EXPECT_TRUE(has_diagonal);
    total += found.count;
  }
  EXPECT_EQ(total, spec::kNonZeros);
}

TEST(SparseRowColumns, MatchTheSystemBuiltFromThePlainStatementOfTheStructure) {
  const evidence::System system = evidence::generated_system(evidence::kSeed, 2);
  ASSERT_EQ(system.row_ptr.size(), spec::kRows + 1);
  for (const std::size_t row :
       {std::size_t{0}, std::size_t{1}, std::size_t{96}, std::size_t{97}, std::size_t{1030},
        std::size_t{1031}, std::size_t{8000}, spec::kRows - 1032, spec::kRows - 1031,
        spec::kRows - 98, spec::kRows - 97, spec::kRows - 2, spec::kRows - 1}) {
    const RowColumns found = row_columns(row);
    ASSERT_EQ(found.count, system.row_ptr[row + 1] - system.row_ptr[row]) << "row " << row;
    for (std::size_t entry = 0; entry < found.count; ++entry) {
      EXPECT_EQ(found.columns.at(entry), system.columns[system.row_ptr[row] + entry]);
    }
  }
}

TEST(SparseClampTolerance, LimitsToTheFiniteAndPositive) {
  constexpr double kMax = std::numeric_limits<double>::max();
  EXPECT_EQ(clamp_tolerance(std::numeric_limits<double>::quiet_NaN()), kMax);
  EXPECT_EQ(clamp_tolerance(std::numeric_limits<double>::infinity()), kMax);
  EXPECT_EQ(clamp_tolerance(kMax), kMax);
  EXPECT_EQ(clamp_tolerance(0.0), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(-0.0), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(-1.0), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(-std::numeric_limits<double>::infinity()), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(std::numeric_limits<double>::denorm_min()), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(spec::kMinTolerance), spec::kMinTolerance);
  EXPECT_EQ(clamp_tolerance(std::nextafter(spec::kMinTolerance, 1.0)),
            std::nextafter(spec::kMinTolerance, 1.0));
  EXPECT_EQ(clamp_tolerance(1.5e-12), 1.5e-12);
  EXPECT_EQ(clamp_tolerance(std::nextafter(kMax, 0.0)), std::nextafter(kMax, 0.0));
  EXPECT_EQ(spec::kMinTolerance, std::numeric_limits<double>::min());
}

TEST(SparseTolerances, AreTheSpecificationsFactorsTimesTheSumWithTheSlack) {
  for (std::size_t entries = 4; entries <= 7; ++entries) {
    for (const double sum : {1.0, 0.25, 37.5}) {
      EXPECT_EQ(row_tolerance(entries, sum),
                spec::row_tolerance_factor(entries) * sum * (1.0 + spec::kEvaluationSlack));
    }
    EXPECT_EQ(row_tolerance(entries, 0.0), spec::kMinTolerance) << "a row with no products floors";
    EXPECT_EQ(row_tolerance(entries, std::numeric_limits<double>::infinity()),
              std::numeric_limits<double>::max());
    EXPECT_EQ(row_tolerance(entries, std::numeric_limits<double>::quiet_NaN()),
              std::numeric_limits<double>::max());
  }
  EXPECT_GT(row_tolerance(7, 1.0), row_tolerance(4, 1.0));
  EXPECT_EQ(residual_norm_tolerance(1.0),
            spec::residual_norm_factor() * 1.0 * (1.0 + spec::kEvaluationSlack));
  EXPECT_EQ(residual_norm_tolerance(128.0), 128.0 * residual_norm_tolerance(1.0));
  EXPECT_EQ(residual_norm_tolerance(0.0), spec::kMinTolerance);
  EXPECT_NEAR(residual_norm_tolerance(1.0), 1.97e-11, 1e-13);
}

TEST(SparseCreate, GivesTheOracleForEveryChunkSize) {
  for (const std::uint32_t chunk : spec::kChunkRows) {
    const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 5, chunk, 0, 0};
    auto created = create(workloads::WorkloadConfig{}, plan);
    ASSERT_TRUE(created.has_value());
    const auto oracle = std::move(created).value();
    EXPECT_EQ(oracle->workload(), "sparse");
    EXPECT_EQ(oracle->contract(), Contract::kBounded);
  }
}

TEST(SparseCreate, RefusesAPlanThatIsNotAUnitOfTheSpecification) {
  const auto refused = [](const workloads::Plan& plan) {
    auto created = create(workloads::WorkloadConfig{}, plan);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  };
  const workloads::Plan good{spec::kInputBytes, spec::kOutputBytes, 0, 1, 1024, 0, 0};
  workloads::Plan plan = good;
  plan.input_bytes = spec::kInputBytes - 1;
  refused(plan);
  plan = good;
  plan.input_bytes = spec::kInputBytes + 4;
  refused(plan);
  plan = good;
  plan.output_bytes = spec::kOutputBytes - 8;
  refused(plan);
  plan = good;
  plan.output_bytes = 0;
  refused(plan);
  for (const std::uint32_t chunk : {0U, 1U, 128U, 1000U, 4096U}) {
    plan = good;
    plan.decomposition = chunk;
    refused(plan);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::sparse
