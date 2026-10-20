// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two register layouts as pure functions. The values are the architecture
// manuals' (MXCSR: Intel SDM vol. 1 §10.2.3, reset value 0x1F80; FPCR: Arm ARM
// D24.2.67, reset value 0) and are compared with the real register in
// real_control_test.cpp on the architecture that has it.
#include "platform/fpenv/registers.hpp"

#include <gtest/gtest.h>

namespace loadforge::fpenv {
namespace {

constexpr std::uint32_t kMxcsrReset = 0x1F80U;  // all exceptions masked, nothing else.

TEST(MxcsrTest, TheResetValueIsTheRequiredState) {
  EXPECT_EQ(decode_mxcsr(kMxcsrReset), required_state(true, false));
}

TEST(MxcsrTest, EachRoundingFieldDecodesAndEncodes) {
  struct Row {
    std::uint32_t field;
    Rounding rounding;
  };
  for (const Row row : {Row{0, Rounding::kToNearestEven}, Row{1, Rounding::kDownward},
                        Row{2, Rounding::kUpward}, Row{3, Rounding::kTowardZero}}) {
    const std::uint32_t reg = (kMxcsrReset & ~(3U << 13)) | (row.field << 13);
    EXPECT_EQ(decode_mxcsr(reg).rounding, row.rounding);
    FpState state = required_state(true, false);
    state.rounding = row.rounding;
    EXPECT_EQ(encode_mxcsr(kMxcsrReset, state), reg);
  }
}

TEST(MxcsrTest, FlagsDecodeFromTheirOwnBits) {
  EXPECT_TRUE(decode_mxcsr(kMxcsrReset | (1U << 15)).flush_to_zero);
  EXPECT_FALSE(decode_mxcsr(kMxcsrReset | (1U << 15)).denormals_are_zero.value());
  EXPECT_TRUE(decode_mxcsr(kMxcsrReset | (1U << 6)).denormals_are_zero.value());
  EXPECT_FALSE(decode_mxcsr(kMxcsrReset | (1U << 6)).flush_to_zero);
  EXPECT_FALSE(decode_mxcsr(kMxcsrReset).default_nan.has_value());
}

TEST(MxcsrTest, EncodeSetsAndClearsEachFlagAndKeepsEveryOtherBit) {
  const std::uint32_t others = 0x1F3FU;  // unmasked-looking mix of exception flags and masks
  FpState state = required_state(true, false);
  state.flush_to_zero = true;
  state.denormals_are_zero = true;
  const std::uint32_t set = encode_mxcsr(others, state);
  EXPECT_EQ(set, others | (1U << 15) | (1U << 6));
  EXPECT_EQ(encode_mxcsr(set, required_state(true, false)), others);
}

TEST(MxcsrTest, AnAbsentDenormalsAreZeroLeavesTheBitAlone) {
  FpState state;
  EXPECT_EQ(encode_mxcsr(kMxcsrReset | (1U << 6), state), kMxcsrReset | (1U << 6));
  EXPECT_EQ(encode_mxcsr(kMxcsrReset, state), kMxcsrReset);
}

TEST(MxcsrTest, ARoundingValueOutsideTheEnumerationLandsOnADefinedField) {
  FpState state = required_state(true, false);
  state.rounding = static_cast<Rounding>(200);  // 200 & 3 == 0
  EXPECT_EQ(encode_mxcsr(kMxcsrReset, state), kMxcsrReset);
}

TEST(FpcrTest, TheResetValueIsTheRequiredState) {
  EXPECT_EQ(decode_fpcr(0), required_state(false, true));
}

TEST(FpcrTest, EachRoundingFieldDecodesAndEncodes) {
  struct Row {
    std::uint64_t field;
    Rounding rounding;
  };
  for (const Row row : {Row{0, Rounding::kToNearestEven}, Row{1, Rounding::kUpward},
                        Row{2, Rounding::kDownward}, Row{3, Rounding::kTowardZero}}) {
    const std::uint64_t reg = row.field << 22;
    EXPECT_EQ(decode_fpcr(reg).rounding, row.rounding);
    FpState state = required_state(false, true);
    state.rounding = row.rounding;
    EXPECT_EQ(encode_fpcr(0, state), reg);
  }
}

TEST(FpcrTest, FlagsDecodeFromTheirOwnBits) {
  EXPECT_TRUE(decode_fpcr(1ULL << 24).flush_to_zero);
  EXPECT_FALSE(decode_fpcr(1ULL << 24).default_nan.value());
  EXPECT_TRUE(decode_fpcr(1ULL << 25).default_nan.value());
  EXPECT_FALSE(decode_fpcr(1ULL << 25).flush_to_zero);
  EXPECT_FALSE(decode_fpcr(0).denormals_are_zero.has_value());
}

TEST(FpcrTest, EncodeSetsAndClearsEachFlagAndKeepsEveryOtherBit) {
  const std::uint64_t others = 0x0000'0000'0000'9F00ULL;  // trap-enable bits
  FpState state = required_state(false, true);
  state.flush_to_zero = true;
  state.default_nan = true;
  const std::uint64_t set = encode_fpcr(others, state);
  EXPECT_EQ(set, others | (1ULL << 24) | (1ULL << 25));
  EXPECT_EQ(encode_fpcr(set, required_state(false, true)), others);
}

TEST(FpcrTest, AnAbsentDefaultNanLeavesTheBitAlone) {
  FpState state;
  EXPECT_EQ(encode_fpcr(1ULL << 25, state), 1ULL << 25);
  EXPECT_EQ(encode_fpcr(0, state), 0ULL);
}

TEST(FpcrTest, ARoundingValueOutsideTheEnumerationLandsOnADefinedField) {
  FpState state = required_state(false, true);
  state.rounding = static_cast<Rounding>(201);  // 201 & 3 == 1, which is toward zero
  EXPECT_EQ(encode_fpcr(0, state), 3ULL << 22);
}

}  // namespace
}  // namespace loadforge::fpenv
