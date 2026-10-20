// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kernels and the reduction tree, on matrices and vectors small enough to
// be worked by hand, and on the bytes a corrupt input would hold.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "workloads/sparse/kernels.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::uint32_t kNone = 0xFFFFFFFFU;

Bytes u32_bytes(const std::vector<std::uint32_t>& words) {
  Bytes bytes(words.size() * sizeof(std::uint32_t));
  for (std::size_t i = 0; i < words.size(); ++i) {
    store_u32(bytes, i, words[i]);
  }
  return bytes;
}

Bytes f64_bytes(const std::vector<double>& values) {
  Bytes bytes(values.size() * sizeof(double));
  for (std::size_t i = 0; i < values.size(); ++i) {
    store_f64(bytes, i, values[i]);
  }
  return bytes;
}

TEST(SparseKernels, TheLoadsAndStoresAreLittleEndianAndUnaligned) {
  Bytes bytes(17, std::byte{0});
  store_f64(std::span(bytes).subspan(1), 0, 1.0);
  EXPECT_EQ(bytes[8], std::byte{0x3F});
  EXPECT_EQ(bytes[7], std::byte{0xF0});
  EXPECT_EQ(load_f64(std::span<const std::byte>(bytes).subspan(1), 0), 1.0);
  store_u32(std::span(bytes).subspan(3), 1, 0x01020304U);
  EXPECT_EQ(bytes[7], std::byte{0x04});
  EXPECT_EQ(bytes[10], std::byte{0x01});
  EXPECT_EQ(load_u32(std::span<const std::byte>(bytes).subspan(3), 1), 0x01020304U);
}

TEST(SparseKernels, ARowIsItsRowPointersWhenTheyAreInRange) {
  const Bytes row_ptr = u32_bytes({0, 4, 9, 9});
  const Matrix matrix{{}, {}, row_ptr};
  EXPECT_EQ(row_range(matrix, 0).first, 0U);
  EXPECT_EQ(row_range(matrix, 0).length, 4U);
  EXPECT_EQ(row_range(matrix, 1).first, 4U);
  EXPECT_EQ(row_range(matrix, 1).length, 5U);
  EXPECT_EQ(row_range(matrix, 2).length, 0U) << "an empty row";
}

TEST(SparseKernels, ARowPointerOutOfOrderOutOfRangeOrHugeIsMadeSafe) {
  struct Case {
    std::uint32_t from;
    std::uint32_t to;
    std::size_t first;
    std::size_t length;
    const char* what;
  };
  const Case cases[] = {
      {10, 5, 10, 0, "the end before the start: empty"},
      {7, 7, 7, 0, "equal pointers: empty"},
      {kNone, 3, kNonZeros, 0, "a start past the end is the end"},
      {kNone, kNone, kNonZeros, 0, "both past the end"},
      {100, kNone, 100, 7, "an end past the end is the end, and a row has at most seven"},
      {kNonZeros - 3, kNone, kNonZeros - 3, 3, "cut at the end of the stored entries"},
      {50, 58, 50, 7, "eight entries are seven"},
      {50, 57, 50, 7, "seven are seven"},
      {50, 56, 50, 6, "six are six"},
      {0, 100000, 0, 7, "a huge row costs a row"},
  };
  for (const Case& c : cases) {
    const Bytes row_ptr = u32_bytes({c.from, c.to});
    const RowRange range = row_range(Matrix{{}, {}, row_ptr}, 0);
    EXPECT_EQ(range.first, c.first) << c.what;
    EXPECT_EQ(range.length, c.length) << c.what;
  }
}

TEST(SparseKernels, TheReductionTreePairsNeighboursAndThenTheirSums) {
  constexpr double big = 9007199254740992.0;  // 2^53: adding 1 is lost, adding 2 is not
  std::array<double, 4> four = {big, 1.0, 1.0, 1.0};
  EXPECT_EQ(reduce_tree(four), (big + 1.0) + (1.0 + 1.0));
  EXPECT_EQ((big + 1.0) + (1.0 + 1.0), big + 2.0);
  std::array<double, 4> sequential = {big, 1.0, 1.0, 1.0};
  double running = sequential[0];
  for (std::size_t i = 1; i < 4; ++i) {
    running += sequential[i];
  }
  EXPECT_NE(running, big + 2.0) << "the tree is not left-to-right";

  const std::array<double, 8> values = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
  std::array<double, 8> eight = values;
  EXPECT_EQ(std::bit_cast<std::uint64_t>(reduce_tree(eight)),
            std::bit_cast<std::uint64_t>(((values[0] + values[1]) + (values[2] + values[3])) +
                                         ((values[4] + values[5]) + (values[6] + values[7]))));
  std::array<double, 1> one = {3.5};
  EXPECT_EQ(reduce_tree(one), 3.5);
  std::array<double, 2> two = {0.1, 0.2};
  EXPECT_EQ(reduce_tree(two), 0.1 + 0.2);
}

TEST(SparseKernels, TheTreeOverSixtyFourPartialsHasTheShapeOfASumOverPairsOfPairs) {
  std::array<double, 64> partials{};
  std::array<double, 64> level{};
  for (std::size_t i = 0; i < 64; ++i) {
    partials[i] = 1.0 / static_cast<double>(i + 3);
    level[i] = partials[i];
  }
  for (std::size_t width = 64; width > 1; width /= 2) {
    std::array<double, 64> next{};
    for (std::size_t i = 0; i < width / 2; ++i) {
      next[i] = level[2 * i] + level[2 * i + 1];
    }
    level = next;
  }
  EXPECT_EQ(std::bit_cast<std::uint64_t>(reduce_tree(partials)),
            std::bit_cast<std::uint64_t>(level[0]));
}

/// A matrix of one entry per row on the diagonal, 2.0, in the shape of the
/// real one for the kernels that read it.
struct Diagonal {
  Bytes values = f64_bytes(std::vector<double>(kRows, 2.0));
  Bytes columns;
  Bytes row_ptr;
  Diagonal() {
    std::vector<std::uint32_t> cols(kRows);
    std::vector<std::uint32_t> ptr(kRows + 1);
    for (std::size_t i = 0; i < kRows; ++i) {
      cols[i] = static_cast<std::uint32_t>(i);
      ptr[i] = static_cast<std::uint32_t>(i);
    }
    ptr[kRows] = kRows;
    columns = u32_bytes(cols);
    row_ptr = u32_bytes(ptr);
  }
  [[nodiscard]] Matrix matrix() const { return Matrix{values, columns, row_ptr}; }
};

struct Vectors {
  std::vector<double> x = std::vector<double>(kRows, -7.0);
  std::vector<double> r = std::vector<double>(kRows, -7.0);
  std::vector<double> p = std::vector<double>(kRows, -7.0);
  std::vector<double> q = std::vector<double>(kRows, -7.0);
  Bytes output = Bytes(kOutputBytes, std::byte{0});
};

Work work_of(const Matrix& matrix, const Bytes& rhs, Vectors& v) {
  return Work{matrix, rhs, v.x, v.r, v.p, v.q, v.output};
}

TEST(SparseKernels, InitSetsTheStartOfTheRecurrenceOnItsRowsAndSumsRTimesR) {
  const Diagonal diagonal;
  std::vector<double> b(kRows);
  for (std::size_t i = 0; i < kRows; ++i) {
    b[i] = static_cast<double>(i % 5) - 2.0;
  }
  const Bytes rhs = f64_bytes(b);
  Vectors v;
  const Work work = work_of(diagonal.matrix(), rhs, v);
  const double partial = scalar_kernels().init(work, Rows{10, 20}, 0.0);
  double expected = 0.0;
  for (std::size_t i = 10; i < 20; ++i) {
    expected += b[i] * b[i];
    EXPECT_EQ(v.x[i], 0.0);
    EXPECT_EQ(v.r[i], b[i]);
    EXPECT_EQ(v.p[i], b[i]);
  }
  EXPECT_EQ(partial, expected);
  EXPECT_EQ(v.x[9], -7.0) << "the rows before the chunk are not touched";
  EXPECT_EQ(v.p[20], -7.0) << "nor the rows after it";
}

TEST(SparseKernels, SpmvDotFormsQAndSumsPTimesQOverItsRows) {
  const Diagonal diagonal;
  const Bytes rhs = f64_bytes(std::vector<double>(kRows, 0.0));
  Vectors v;
  for (std::size_t i = 0; i < kRows; ++i) {
    v.p[i] = static_cast<double>(i) * 0.5;
  }
  const Work work = work_of(diagonal.matrix(), rhs, v);
  const double partial = scalar_kernels().spmv_dot(work, Rows{100, 104}, 0.0);
  double expected = 0.0;
  for (std::size_t i = 100; i < 104; ++i) {
    EXPECT_EQ(v.q[i], 2.0 * v.p[i]);
    expected += v.p[i] * v.q[i];
  }
  EXPECT_EQ(partial, expected);
  EXPECT_EQ(v.q[99], -7.0);
  EXPECT_EQ(v.q[104], -7.0);
}

TEST(SparseKernels, AColumnPastTheVectorGathersFromTheMaskedIndexNotFromOutsideIt) {
  Diagonal diagonal;
  store_u32(diagonal.columns, 5, 0xFFFFFFFFU);        // masked: kRows - 1
  store_u32(diagonal.columns, 6, kRows + 2);          // masked: 2
  const Bytes rhs = f64_bytes(std::vector<double>(kRows, 0.0));
  Vectors v;
  for (std::size_t i = 0; i < kRows; ++i) {
    v.p[i] = static_cast<double>(i);
  }
  const Work work = work_of(diagonal.matrix(), rhs, v);
  (void)scalar_kernels().spmv_dot(work, Rows{4, 8}, 0.0);
  EXPECT_EQ(v.q[5], 2.0 * static_cast<double>(kRows - 1));
  EXPECT_EQ(v.q[6], 2.0 * 2.0);
  EXPECT_EQ(v.q[4], 2.0 * 4.0);
}

TEST(SparseKernels, UpdateXrMovesXAndRAndSumsTheNewRSquared) {
  const Diagonal diagonal;
  const Bytes rhs = f64_bytes(std::vector<double>(kRows, 0.0));
  Vectors v;
  for (std::size_t i = 0; i < kRows; ++i) {
    v.x[i] = 1.0;
    v.r[i] = 3.0;
    v.p[i] = 0.5;
    v.q[i] = 2.0;
  }
  const Work work = work_of(diagonal.matrix(), rhs, v);
  const double partial = scalar_kernels().update_xr(work, Rows{8, 11}, 0.25);
  for (std::size_t i = 8; i < 11; ++i) {
    EXPECT_EQ(v.x[i], 1.0 + 0.25 * 0.5);
    EXPECT_EQ(v.r[i], 3.0 - 0.25 * 2.0);
  }
  EXPECT_EQ(partial, 3 * (2.5 * 2.5));
  EXPECT_EQ(v.x[7], 1.0);
  EXPECT_EQ(v.r[11], 3.0);
}

TEST(SparseKernels, UpdatePSetsPToRPlusBetaPAndHasNoPartial) {
  const Diagonal diagonal;
  const Bytes rhs = f64_bytes(std::vector<double>(kRows, 0.0));
  Vectors v;
  for (std::size_t i = 0; i < kRows; ++i) {
    v.r[i] = 3.0;
    v.p[i] = 0.5;
  }
  const Work work = work_of(diagonal.matrix(), rhs, v);
  EXPECT_EQ(scalar_kernels().update_p(work, Rows{0, 2}, 4.0), 0.0);
  EXPECT_EQ(v.p[0], 3.0 + 4.0 * 0.5);
  EXPECT_EQ(v.p[1], 5.0);
  EXPECT_EQ(v.p[2], 0.5);
}

TEST(SparseKernels, FinishWritesXThenTheExplicitResidualOfItsRowsOnly) {
  const Diagonal diagonal;
  std::vector<double> b(kRows);
  Vectors v;
  for (std::size_t i = 0; i < kRows; ++i) {
    b[i] = 10.0 + static_cast<double>(i);
    v.x[i] = 1.5;
  }
  const Bytes rhs = f64_bytes(b);
  const Work work = work_of(diagonal.matrix(), rhs, v);
  EXPECT_EQ(scalar_kernels().finish(work, Rows{3, 5}, 0.0), 0.0);
  const std::span<const std::byte> out(v.output);
  for (std::size_t i = 3; i < 5; ++i) {
    EXPECT_EQ(load_f64(out, i), 1.5);
    EXPECT_EQ(load_f64(out, kRows + i), b[i] - 2.0 * 1.5);
  }
  EXPECT_EQ(load_f64(out, 2), 0.0) << "rows outside the chunk are not written";
  EXPECT_EQ(load_f64(out, kRows + 5), 0.0);
}

TEST(SparseKernels, NaNAndInfinityAreArithmeticAndSpreadThroughASum) {
  const Diagonal diagonal;
  std::vector<double> b(kRows, 1.0);
  b[12] = std::numeric_limits<double>::quiet_NaN();
  const Bytes rhs = f64_bytes(b);
  Vectors v;
  const Work work = work_of(diagonal.matrix(), rhs, v);
  EXPECT_TRUE(std::isnan(scalar_kernels().init(work, Rows{10, 14}, 0.0)));
  EXPECT_EQ(scalar_kernels().init(work, Rows{0, 4}, 0.0), 4.0);
}

}  // namespace
}  // namespace loadforge::workloads::sparse
