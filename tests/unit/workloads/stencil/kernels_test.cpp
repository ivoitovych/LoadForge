// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sweep kernels, one rectangle at a time, against arithmetic done by hand.

#include "workloads/stencil/kernels.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/stencil/stencil_support.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;

constexpr std::uint64_t kSentinelBits = 0x7FF8DEADBEEF1234ULL;  // a NaN with a payload

Bytes sentinel_grid() {
  Bytes grid(kGridBytes);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    std::memcpy(grid.data() + cell * 8, &kSentinelBits, 8);
  }
  return grid;
}

std::uint64_t bits_at(const Bytes& grid, std::size_t row, std::size_t column) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, grid.data() + (row * kEdge + column) * 8, 8);
  return bits;
}

SweepKernel kernel_for(simd::IsaPath path) { return simd::kernel_at(kernel_table(), path); }

TEST(StencilKernelTable, ScalarIsAlwaysThereAndTheBuildsOtherPathsAreTheCompiledOnes) {
  const simd::KernelTable<SweepKernel> table = kernel_table();
  EXPECT_NE(table.scalar, nullptr);
  EXPECT_EQ(table.avx512, nullptr) << "no AVX-512 kernel: recorded in the journal";
  EXPECT_EQ(simd::kernel_paths(table), simd::compiled_paths().intersect(simd::kernel_paths(table)));
  EXPECT_TRUE(simd::kernel_paths(table).contains(simd::IsaPath::kScalar));
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(simd::kernel_paths(table), (simd::IsaSet{simd::IsaPath::kScalar}));
#elif defined(__x86_64__)
  EXPECT_EQ(simd::kernel_paths(table),
            (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2}));
#elif defined(__aarch64__)
  EXPECT_EQ(simd::kernel_paths(table),
            (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kNeon}));
#endif
}

class StencilKernel : public ::testing::TestWithParam<simd::IsaPath> {};

/// u = 8, N = 16, S = 24, W = 32, E = 40: 8/2 + (16 + 24 + 32 + 40) / 8 = 18.
TEST_P(StencilKernel, AnInteriorCellIsHalfItselfPlusAnEighthOfItsFourNeighbours) {
  Bytes src(kGridBytes);
  testing::set_cell(src, 10, 10, 8.0);
  testing::set_cell(src, 9, 10, 16.0);
  testing::set_cell(src, 11, 10, 24.0);
  testing::set_cell(src, 10, 9, 32.0);
  testing::set_cell(src, 10, 11, 40.0);
  Bytes dst = sentinel_grid();
  kernel_for(GetParam())(src, dst, Rect{10, 11, 10, 11});
  EXPECT_EQ(testing::cell_of(dst, 10, 10), 18.0);
}

/// Cells at distinct powers of two make each weight visible: a swapped
/// neighbour or a wrong weight changes the answer.
TEST_P(StencilKernel, EveryNeighbourCarriesItsOwnWeight) {
  const std::size_t row = 77;
  const std::size_t column = 130;
  const struct {
    std::size_t row;
    std::size_t column;
    double value;
    double weight;
  } terms[] = {{row, column, 1.0, 0.5},
               {row - 1, column, 2.0, 0.125},
               {row + 1, column, 4.0, 0.125},
               {row, column - 1, 8.0, 0.125},
               {row, column + 1, 16.0, 0.125}};
  double expected = 0.0;
  for (const auto& term : terms) {
    Bytes src(kGridBytes);
    testing::set_cell(src, term.row, term.column, term.value);
    Bytes dst = sentinel_grid();
    kernel_for(GetParam())(src, dst, Rect{row, row + 1, column, column + 1});
    EXPECT_EQ(testing::cell_of(dst, row, column), term.value * term.weight)
        << term.row << "," << term.column;
    expected += term.value * term.weight;
  }
  Bytes src(kGridBytes);
  for (const auto& term : terms) {
    testing::set_cell(src, term.row, term.column, term.value);
  }
  Bytes dst = sentinel_grid();
  kernel_for(GetParam())(src, dst, Rect{row, row + 1, column, column + 1});
  EXPECT_EQ(testing::cell_of(dst, row, column), expected);
}

TEST_P(StencilKernel, ARectangleIsWrittenWholeAndNothingAroundIt) {
  // Every width from one cell to past two vectors, at every alignment of its
  // start, so the lane loop, its tail and their seam are each taken.
  const Bytes src = testing::random_grid(5, 1);
  for (std::size_t width = 1; width <= 11; ++width) {
    for (std::size_t offset = 0; offset < 5; ++offset) {
      const Rect rect{40, 43, 60 + offset, 60 + offset + width};
      Bytes dst = sentinel_grid();
      kernel_for(GetParam())(src, dst, rect);
      Bytes reference = sentinel_grid();
      kernel_for(simd::IsaPath::kScalar)(src, reference, rect);
      EXPECT_EQ(dst, reference) << "width " << width << " offset " << offset;
      for (std::size_t row = 39; row <= 43; ++row) {
        for (std::size_t column = 59; column <= 60 + offset + width; ++column) {
          const bool inside = row >= rect.row_begin && row < rect.row_end &&
                              column >= rect.column_begin && column < rect.column_end;
          EXPECT_EQ(bits_at(dst, row, column) == kSentinelBits, !inside)
              << row << "," << column << " width " << width << " offset " << offset;
        }
      }
    }
  }
}

TEST_P(StencilKernel, TheLastInteriorRowAndColumnAreReadAgainstTheRing) {
  const Bytes src = testing::random_grid(6, 2);
  Bytes dst = sentinel_grid();
  kernel_for(GetParam())(src, dst, Rect{1, kEdge - 1, 1, kEdge - 1});
  for (std::size_t row = 1; row + 1 < kEdge; ++row) {
    for (std::size_t column = 1; column + 1 < kEdge; ++column) {
      const double expected =
          testing::cell_of(src, row, column) * 0.5 +
          ((testing::cell_of(src, row - 1, column) + testing::cell_of(src, row + 1, column)) +
           (testing::cell_of(src, row, column + 1) + testing::cell_of(src, row, column - 1))) *
              0.125;
      ASSERT_EQ(std::bit_cast<std::uint64_t>(testing::cell_of(dst, row, column)),
                std::bit_cast<std::uint64_t>(expected))
          << row << "," << column;
    }
  }
  for (std::size_t at = 0; at < kEdge; ++at) {
    EXPECT_EQ(bits_at(dst, 0, at), kSentinelBits) << "the ring is the caller's";
    EXPECT_EQ(bits_at(dst, kEdge - 1, at), kSentinelBits);
    EXPECT_EQ(bits_at(dst, at, 0), kSentinelBits);
    EXPECT_EQ(bits_at(dst, at, kEdge - 1), kSentinelBits);
  }
}

std::vector<simd::IsaPath> paths_under_test() { return testing::runnable_paths(); }

INSTANTIATE_TEST_SUITE_P(Paths, StencilKernel, ::testing::ValuesIn(paths_under_test()),
                         [](const ::testing::TestParamInfo<simd::IsaPath>& info) {
                           return std::string(simd::describe(info.param));
                         });

}  // namespace
}  // namespace loadforge::workloads::stencil
