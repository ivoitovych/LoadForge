// SPDX-License-Identifier: GPL-3.0-or-later
//
// What generate() writes, read back through the format spec.hpp fixes: the
// parts of the input, the structure of the matrix, its symmetry and ranges.
// The values are checked against the generator's own definition in the oracle
// tier, which builds the matrix a second way.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "workloads/sparse/kernels.hpp"
#include "workloads/sparse/sparse.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using Bytes = std::vector<std::byte>;

Bytes unit_input(std::uint64_t seed, std::uint64_t index) {
  const auto created = create(WorkloadConfig{seed, kInputBytes + kOutputBytes, 0, {}});
  EXPECT_TRUE(created.has_value());
  Bytes input(kInputBytes, std::byte{0xA5});
  created.value()->generate(index, input);
  return input;
}

TEST(SparseGenerate, TheRowPointersAreTheCumulativeRowLengthsAndEndAtTheNonZeroCount) {
  const Bytes input = unit_input(1, 0);
  const std::span<const std::byte> row_ptr =
      std::span(input).subspan(kRowPtrOffset, (kRows + 1) * sizeof(std::uint32_t));
  std::size_t expected = 0;
  for (std::size_t row = 0; row < kRows; ++row) {
    ASSERT_EQ(load_u32(row_ptr, row), expected) << "row " << row;
    expected += row_length(row);
  }
  EXPECT_EQ(load_u32(row_ptr, kRows), kNonZeros);
  EXPECT_EQ(expected, kNonZeros);
}

TEST(SparseGenerate, EachRowHoldsItsColumnsInIncreasingOrderWithTheDiagonalWhereItBelongs) {
  const Bytes input = unit_input(2, 3);
  const std::span<const std::byte> columns =
      std::span(input).subspan(kColumnsOffset, kNonZeros * sizeof(std::uint32_t));
  const std::span<const std::byte> values =
      std::span(input).subspan(kValuesOffset, kNonZeros * sizeof(double));
  std::size_t entry = 0;
  for (std::size_t row = 0; row < kRows; ++row) {
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
      ASSERT_EQ(load_u32(columns, entry), column) << "row " << row << " entry " << entry;
      if (column == row) {
        ASSERT_EQ(load_f64(values, entry), kDiagonal);
      } else {
        ASSERT_LE(std::fabs(load_f64(values, entry)), 0.125);
      }
      ++entry;
    }
  }
  EXPECT_EQ(entry, kNonZeros);
}

TEST(SparseGenerate, TheMatrixIsSymmetric) {
  const Bytes input = unit_input(3, 1);
  const std::span<const std::byte> columns =
      std::span(input).subspan(kColumnsOffset, kNonZeros * sizeof(std::uint32_t));
  const std::span<const std::byte> values =
      std::span(input).subspan(kValuesOffset, kNonZeros * sizeof(double));
  const std::span<const std::byte> row_ptr =
      std::span(input).subspan(kRowPtrOffset, (kRows + 1) * sizeof(std::uint32_t));
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t entry = load_u32(row_ptr, row); entry < load_u32(row_ptr, row + 1); ++entry) {
      const std::size_t column = load_u32(columns, entry);
      bool found = false;
      for (std::size_t other = load_u32(row_ptr, column); other < load_u32(row_ptr, column + 1);
           ++other) {
        if (load_u32(columns, other) == row) {
          ASSERT_EQ(load_f64(values, other), load_f64(values, entry)) << row << "," << column;
          found = true;
        }
      }
      ASSERT_TRUE(found) << "no mirror of (" << row << "," << column << ")";
    }
  }
}

TEST(SparseGenerate, TheRightHandSideIsInTheRangeTheSpecificationGives) {
  const Bytes input = unit_input(4, 2);
  const std::span<const std::byte> rhs =
      std::span(input).subspan(kRhsOffset, kRows * sizeof(double));
  double lowest = 0.0;
  double highest = 0.0;
  for (std::size_t row = 0; row < kRows; ++row) {
    const double b = load_f64(rhs, row);
    ASSERT_GE(b, -1.0);
    ASSERT_LT(b, 1.0);
    ASSERT_EQ(b * 4503599627370496.0, std::nearbyint(b * 4503599627370496.0)) << "a multiple of 2^-52";
    lowest = std::min(lowest, b);
    highest = std::max(highest, b);
  }
  EXPECT_LT(lowest, -0.99);
  EXPECT_GT(highest, 0.99);
}

TEST(SparseGenerate, EveryByteIsWrittenAndTheSeedAndTheUnitSelectTheData) {
  const auto created = create(WorkloadConfig{5, kInputBytes + kOutputBytes, 0, {}});
  ASSERT_TRUE(created.has_value());
  Bytes ones(kInputBytes, std::byte{0xFF});
  Bytes zeros(kInputBytes, std::byte{0x00});
  created.value()->generate(0, ones);
  created.value()->generate(0, zeros);
  EXPECT_EQ(ones, zeros);
  EXPECT_NE(unit_input(5, 0), unit_input(6, 0));
  EXPECT_NE(unit_input(5, 0), unit_input(5, 1));
  EXPECT_EQ(unit_input(5, 0), ones);
}

}  // namespace
}  // namespace loadforge::workloads::sparse
