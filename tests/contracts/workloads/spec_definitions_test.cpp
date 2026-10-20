// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the two workload specifications carry.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "workloads/compression/spec.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads {
namespace {

TEST(CompressionSpecDefinition, TheBoundIsLz4sAndTheHeaderIsEightBytes) {
  EXPECT_EQ(compression::compress_bound(0), 16U);
  EXPECT_EQ(compression::compress_bound(254), 270U);
  EXPECT_EQ(compression::compress_bound(255), 272U);
  EXPECT_EQ(compression::compress_bound(compression::kInputBytes), 65536U + 257U + 16U);
  EXPECT_EQ(compression::output_bytes_for(compression::kInputBytes), 8U + 65809U);
}

TEST(CompressionSpecDefinition, LittleEndianWordsAndTheMagicSpellLfz1) {
  const std::array<std::byte, 6> bytes = {std::byte{0xFF}, std::byte{'L'}, std::byte{'F'},
                                          std::byte{'Z'},  std::byte{'1'}, std::byte{0xFF}};
  EXPECT_EQ(compression::load_le32(bytes, 1), compression::kMagic);
  EXPECT_EQ(compression::load_le32(bytes, 2), 0xFF315A46U);
}

TEST(DenseLinearSpecDefinition, EntriesAreExactMultiplesOfTwoToMinus52InTheHalfOpenUnitBall) {
  using dense_linear::entry_value;
  EXPECT_EQ(entry_value(0), -1.0);
  EXPECT_EQ(entry_value(std::uint64_t{1} << 63U), 0.0);
  EXPECT_EQ(entry_value(~std::uint64_t{0}), 1.0 - 1.0 / 4503599627370496.0);
  EXPECT_EQ(entry_value(std::uint64_t{1} << 11U), -1.0 + 1.0 / 4503599627370496.0);
  EXPECT_EQ(entry_value(0x7FFU), -1.0) << "the low 11 bits are discarded";
}

TEST(DenseLinearSpecDefinition, PanelDepthsAndSizes) {
  for (const std::uint32_t kc : {16U, 32U, 64U, 128U, 256U}) {
    EXPECT_TRUE(dense_linear::is_panel_depth(kc)) << kc;
  }
  for (const std::uint32_t kc : {0U, 1U, 48U, 512U}) {
    EXPECT_FALSE(dense_linear::is_panel_depth(kc)) << kc;
  }
  EXPECT_TRUE(dense_linear::is_panel_depth(dense_linear::kDefaultPanelDepth));
  EXPECT_EQ(dense_linear::kInputBytes, 2U * 256U * 256U * 8U);
  EXPECT_EQ(dense_linear::kOutputBytes, 256U * 256U * 8U);
}

TEST(DenseLinearSpecDefinition, GammaIsNuOverOneMinusNu) {
  EXPECT_EQ(dense_linear::gamma(0), 0.0);
  const double nu = 256.0 / 9007199254740992.0;
  EXPECT_EQ(dense_linear::gamma(256), nu / (1.0 - nu));
  EXPECT_GT(dense_linear::gamma(256), 256.0 * dense_linear::kUnitRoundoff);
}

}  // namespace
}  // namespace loadforge::workloads
