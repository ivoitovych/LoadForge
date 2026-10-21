// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the sparse specification carries.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "workloads/sparse/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

TEST(SparseSpecDefinition, TheNameIsAValidWorkloadNameAndTheConstantsAreTheUnitsShape) {
  EXPECT_EQ(sparse::kName, "sparse");
  EXPECT_TRUE(is_valid_name(sparse::kName));
  EXPECT_EQ(sparse::kVersion, 1U);
  EXPECT_EQ(sparse::kRows, 16384U);
  EXPECT_EQ(sparse::kIterations, 16U);
  EXPECT_EQ(sparse::kDiagonal, 2.0);
  EXPECT_EQ(sparse::kOffsets, (std::array<std::size_t, 3>{1, 97, 1031}));
  EXPECT_TRUE(std::ranges::is_sorted(sparse::kOffsets, std::less<>{}));
}

TEST(SparseSpecDefinition, GershgorinKeepsTheMatrixPositiveDefiniteAndWellConditioned) {
  const double off_diagonal_sum =
      static_cast<double>(2 * sparse::kOffsets.size()) * 0.125;  // six entries of at most 1/8
  EXPECT_EQ(off_diagonal_sum, 0.75);
  EXPECT_EQ(sparse::kDiagonal - off_diagonal_sum, 1.25);
  EXPECT_EQ((sparse::kDiagonal + off_diagonal_sum) / (sparse::kDiagonal - off_diagonal_sum), 2.2);
  EXPECT_LE(std::sqrt(11.0 / 5.0), 1.5) << "derivation 2 takes sqrt(kappa) <= 3/2";
  EXPECT_LE((std::sqrt(11.0 / 5.0) - 1.0) / (std::sqrt(11.0 / 5.0) + 1.0), 0.2)
      << "and the rate at most 1/5";
}

TEST(SparseSpecDefinition, RowLengthCountsTheDiagonalAndTheOffsetsThatFit) {
  EXPECT_EQ(sparse::row_length(0), 4U);
  EXPECT_EQ(sparse::row_length(1), 5U) << "the left neighbour at distance 1";
  EXPECT_EQ(sparse::row_length(96), 5U) << "just short of the offset 97 on the left";
  EXPECT_EQ(sparse::row_length(97), 6U);
  EXPECT_EQ(sparse::row_length(1030), 6U) << "just short of 1031 on the left";
  EXPECT_EQ(sparse::row_length(1031), 7U);
  EXPECT_EQ(sparse::row_length(8000), 7U);
  EXPECT_EQ(sparse::row_length(sparse::kRows - 1031 - 1), 7U);
  EXPECT_EQ(sparse::row_length(sparse::kRows - 1031), 6U) << "1031 to the right no longer fits";
  EXPECT_EQ(sparse::row_length(sparse::kRows - 2), 5U);
  EXPECT_EQ(sparse::row_length(sparse::kRows - 1), 4U);
}

TEST(SparseSpecDefinition, TheNonZeroCountIsTheSumOfTheRowLengths) {
  std::size_t total = 0;
  std::size_t shortest = 99;
  std::size_t longest = 0;
  for (std::size_t row = 0; row < sparse::kRows; ++row) {
    const std::size_t length = sparse::row_length(row);
    total += length;
    shortest = std::min(shortest, length);
    longest = std::max(longest, length);
  }
  EXPECT_EQ(total, sparse::kNonZeros);
  EXPECT_EQ(sparse::kNonZeros, 112430U);
  EXPECT_EQ(shortest, 4U);
  EXPECT_EQ(longest, 7U);
}

TEST(SparseSpecDefinition, TheInputIsValuesThenRhsThenColumnsThenRowPointers) {
  EXPECT_EQ(sparse::kValuesOffset, 0U);
  EXPECT_EQ(sparse::kRhsOffset, 112430U * 8U);
  EXPECT_EQ(sparse::kColumnsOffset, sparse::kRhsOffset + std::size_t{16384} * 8);
  EXPECT_EQ(sparse::kRowPtrOffset, sparse::kColumnsOffset + std::size_t{112430} * 4);
  EXPECT_EQ(sparse::kInputBytes, sparse::kRowPtrOffset + std::size_t{16385} * 4);
  EXPECT_EQ(sparse::kRhsOffset, 899440U);
  EXPECT_EQ(sparse::kColumnsOffset, 1030512U);
  EXPECT_EQ(sparse::kRowPtrOffset, 1480232U);
  EXPECT_EQ(sparse::kInputBytes, 1545772U);
  EXPECT_EQ(sparse::kOutputBytes, 2U * 16384U * 8U);
  EXPECT_EQ(sparse::kRhsOffset % alignof(double), 0U);
  EXPECT_EQ(sparse::kColumnsOffset % alignof(std::uint32_t), 0U);
  EXPECT_EQ(sparse::kRowPtrOffset % alignof(std::uint32_t), 0U);
}

TEST(SparseSpecDefinition, EntriesAreMultiplesOfTwoToMinus52AndOffDiagonalsOfTwoToMinus55) {
  using sparse::entry_value;
  using sparse::off_diagonal_value;
  EXPECT_EQ(entry_value(0), -1.0) << "the lower end is reached";
  EXPECT_EQ(entry_value(std::uint64_t{1} << 63U), 0.0);
  EXPECT_EQ(entry_value(~std::uint64_t{0}), 1.0 - 1.0 / 4503599627370496.0);
  EXPECT_EQ(entry_value(0x7FFU), -1.0) << "the low 11 bits are discarded";
  EXPECT_EQ(off_diagonal_value(0), -0.125) << "the lower end, -1/8, is reached";
  EXPECT_EQ(off_diagonal_value(std::uint64_t{1} << 63U), 0.0);
  EXPECT_EQ(off_diagonal_value(~std::uint64_t{0}), (1.0 - 1.0 / 4503599627370496.0) / 8.0);
  EXPECT_LT(off_diagonal_value(~std::uint64_t{0}), 0.125) << "the upper end, +1/8, is not";
}

TEST(SparseSpecDefinition, ChunkRowsDivideTheRowsIntoAPowerOfTwoNumberOfChunks) {
  for (const std::uint32_t rows : {256U, 512U, 1024U, 2048U}) {
    EXPECT_TRUE(sparse::is_chunk_rows(rows)) << rows;
    const std::size_t chunks = sparse::kRows / rows;
    EXPECT_EQ(chunks * rows, sparse::kRows) << rows;
    EXPECT_EQ(chunks & (chunks - 1), 0U)
        << rows << ": the combining tree is a complete binary tree";
  }
  for (const std::uint32_t rows : {0U, 1U, 128U, 1000U, 4096U, 16384U}) {
    EXPECT_FALSE(sparse::is_chunk_rows(rows)) << rows;
  }
  EXPECT_TRUE(sparse::is_chunk_rows(sparse::kDefaultChunkRows));
  EXPECT_EQ(sparse::kDefaultChunkRows, 1024U);
}

TEST(SparseSpecDefinition, GammaIsKuOverOneMinusKu) {
  EXPECT_EQ(sparse::kUnitRoundoff, std::ldexp(1.0, -53));
  EXPECT_EQ(sparse::gamma(0), 0.0);
  const double ku = 8.0 * sparse::kUnitRoundoff;
  EXPECT_EQ(sparse::gamma(8), ku / (1.0 - ku));
  EXPECT_GT(sparse::gamma(8), 8.0 * sparse::kUnitRoundoff);
  EXPECT_LT(sparse::gamma(sparse::kRows + 1), std::ldexp(1.0, -38))
      << "derivation 2: the norms' evaluation error";
}

TEST(SparseSpecDefinition, TheRowToleranceFactorIsTwoGammaOverOneMinusGamma) {
  for (const std::size_t entries : {std::size_t{4}, std::size_t{7}}) {
    const double g = sparse::gamma(entries + 1);
    EXPECT_EQ(sparse::row_tolerance_factor(entries), 2.0 * g / (1.0 - g)) << entries;
  }
  EXPECT_NEAR(sparse::row_tolerance_factor(4), 1.11022302e-15, 1e-23);
  EXPECT_NEAR(sparse::row_tolerance_factor(7), 1.77635684e-15, 1e-23);
  EXPECT_LT(sparse::row_tolerance_factor(4), sparse::row_tolerance_factor(7));
}

TEST(SparseSpecDefinition, TheResidualNormFactorIsThreeOverFiveToTheK) {
  EXPECT_EQ(sparse::residual_norm_factor(), 3.0 / 152587890625.0) << "5^16 = 152587890625";
  EXPECT_NEAR(sparse::residual_norm_factor(), 1.96608e-11, 1e-17);
}

TEST(SparseSpecDefinition, TheFloorAndTheSlackArePinned) {
  EXPECT_EQ(sparse::kMinTolerance, std::numeric_limits<double>::min());
  EXPECT_EQ(sparse::kMinTolerance, std::ldexp(1.0, -1022));
  EXPECT_EQ(sparse::kEvaluationSlack, std::ldexp(1.0, -36));
}

}  // namespace
}  // namespace loadforge::workloads
