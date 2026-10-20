// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real register. FpControlContract runs against it, and the layout the
// code assumes is compared with the hardware by reading the register back with
// an instruction of this test's own, not through the code under test (F21).
#include "platform/fpenv/real_control.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "contracts/fpenv/fpenv_contract.hpp"

namespace loadforge::fpenv {
namespace {

#if defined(__x86_64__)
std::uint64_t raw_register() {
  std::uint32_t value = 0;
  asm volatile("stmxcsr %0" : "=m"(value));
  return value;
}
constexpr unsigned kRoundingShift = 13;
constexpr std::uint64_t kFlushBit = 1U << 15;
// RC encodings from the SDM: 00 nearest, 01 down, 10 up, 11 toward zero.
constexpr std::uint64_t kNearest = 0;
constexpr std::uint64_t kDown = 1;
constexpr std::uint64_t kUp = 2;
constexpr std::uint64_t kZero = 3;
#else
std::uint64_t raw_register() {
  std::uint64_t value = 0;
  asm volatile("mrs %0, fpcr" : "=r"(value));
  return value;
}
constexpr unsigned kRoundingShift = 22;
constexpr std::uint64_t kFlushBit = 1ULL << 24;
// RMode encodings from the Arm ARM: 00 nearest, 01 up, 10 down, 11 toward zero.
constexpr std::uint64_t kNearest = 0;
constexpr std::uint64_t kUp = 1;
constexpr std::uint64_t kDown = 2;
constexpr std::uint64_t kZero = 3;
#endif

class RealRegisterTest : public ::testing::Test {
 protected:
  void SetUp() override { saved_ = control_.read(); }
  void TearDown() override { control_.write(saved_); }

  RealFpControl control_;
  FpState saved_;
};

TEST_F(RealRegisterTest, EachRoundingModeIsTheFieldTheManualNames) {
  struct Row {
    Rounding rounding;
    std::uint64_t field;
  };
  for (const Row row : {Row{Rounding::kToNearestEven, kNearest}, Row{Rounding::kTowardZero, kZero},
                        Row{Rounding::kUpward, kUp}, Row{Rounding::kDownward, kDown}}) {
    FpState state =
        required_state(RealFpControl::kHasDenormalsAreZero, RealFpControl::kHasDefaultNan);
    state.rounding = row.rounding;
    control_.write(state);
    EXPECT_EQ((raw_register() >> kRoundingShift) & 3U, row.field) << describe(row.rounding);
  }
}

TEST_F(RealRegisterTest, FlushToZeroIsTheBitTheManualNames) {
  FpState state =
      required_state(RealFpControl::kHasDenormalsAreZero, RealFpControl::kHasDefaultNan);
  state.flush_to_zero = true;
  control_.write(state);
  EXPECT_NE(raw_register() & kFlushBit, 0U);
  state.flush_to_zero = false;
  control_.write(state);
  EXPECT_EQ(raw_register() & kFlushBit, 0U);
}

TEST_F(RealRegisterTest, AWriteChangesNoBitOutsideTheFourProperties) {
  const std::uint64_t before = raw_register();
  FpState state = control_.read();
  state.rounding = Rounding::kUpward;
  state.flush_to_zero = true;
  control_.write(state);
#if defined(__x86_64__)
  constexpr std::uint64_t mask = ~((3ULL << kRoundingShift) | kFlushBit | (1ULL << 6));
#else
  constexpr std::uint64_t mask = ~((3ULL << kRoundingShift) | kFlushBit | (1ULL << 25));
#endif
  EXPECT_EQ(raw_register() & mask, before & mask);
}

}  // namespace
}  // namespace loadforge::fpenv

namespace loadforge::contracts {
namespace {

struct RealControlSubject {
  static constexpr bool kHasDenormalsAreZero = fpenv::RealFpControl::kHasDenormalsAreZero;
  static constexpr bool kHasDefaultNan = fpenv::RealFpControl::kHasDefaultNan;
  static std::unique_ptr<fpenv::FpControl> make() {
    return std::make_unique<fpenv::RealFpControl>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Real, FpControlContract, ::testing::Types<RealControlSubject>);

}  // namespace
}  // namespace loadforge::contracts
