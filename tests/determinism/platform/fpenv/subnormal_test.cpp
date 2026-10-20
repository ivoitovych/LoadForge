// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: once the state is established, a subnormal computation gives the IEEE 754
// answer, on this architecture; and the same computation under the state the
// module exists to refuse gives a different one, so these tests can fail.
// Operands go through volatile so the compiler cannot fold them under the
// default state at build time.
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <thread>

#include "platform/fpenv/environment.hpp"
#include "platform/fpenv/real_control.hpp"

namespace loadforge::fpenv {
namespace {

constexpr std::uint64_t kHalfMinNormalBits = 0x0008'0000'0000'0000ULL;  // 2^-1023, subnormal
constexpr std::uint64_t kDenormMinBits = 1ULL;                          // 2^-1074

class SubnormalTest : public ::testing::Test {
 protected:
  void SetUp() override { saved_ = control_.read(); }
  void TearDown() override { control_.write(saved_); }

  FpState required() const {
    return required_state(RealFpControl::kHasDenormalsAreZero, RealFpControl::kHasDefaultNan);
  }

  RealFpControl control_;
  FpState saved_;
};

double half_of_min_normal() {
  volatile double min_normal = 0x1p-1022;
  volatile double half = 0.5;
  volatile double result = min_normal * half;
  return result;
}

double denorm_min_times_one() {
  volatile double tiny = 0x1p-1074;
  volatile double one = 1.0;
  volatile double result = tiny * one;
  return result;
}

float float_half_of_min_normal() {
  volatile float min_normal = 0x1p-126F;
  volatile float half = 0.5F;
  volatile float result = min_normal * half;
  return result;
}

TEST_F(SubnormalTest, TheEstablishedStateGivesTheIeeeSubnormalAnswers) {
  ASSERT_TRUE(establish_environment(control_).has_value());
  EXPECT_EQ(std::bit_cast<std::uint64_t>(half_of_min_normal()), kHalfMinNormalBits);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(denorm_min_times_one()), kDenormMinBits);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(float_half_of_min_normal()), 0x0040'0000U);
}

TEST_F(SubnormalTest, FlushToZeroTurnsASubnormalResultIntoZero) {
  FpState state = required();
  state.flush_to_zero = true;
  control_.write(state);
  EXPECT_EQ(half_of_min_normal(), 0.0);
  EXPECT_EQ(float_half_of_min_normal(), 0.0F);
  ASSERT_TRUE(establish_environment(control_).has_value());
  EXPECT_NE(half_of_min_normal(), 0.0) << "and establishing restores the IEEE answer";
}

TEST_F(SubnormalTest, DenormalsAreZeroTurnsASubnormalInputIntoZeroWhereTheArchitectureHasIt) {
  if constexpr (!RealFpControl::kHasDenormalsAreZero) {
    GTEST_SKIP() << "AArch64 has one flag, FZ, covering inputs and outputs (tested above)";
  } else {
    FpState state = required();
    state.denormals_are_zero = true;
    control_.write(state);
    EXPECT_EQ(denorm_min_times_one(), 0.0);
    ASSERT_TRUE(establish_environment(control_).has_value());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(denorm_min_times_one()), kDenormMinBits);
  }
}

TEST_F(SubnormalTest, TheRoundingModeChangesTheLastBitAndEstablishingRestoresNearest) {
  const auto sum_with_tiny = [] {
    volatile double one = 1.0;
    volatile double tiny = 0x1p-80;
    volatile double result = one + tiny;
    return result;
  };
  FpState state = required();
  state.rounding = Rounding::kUpward;
  control_.write(state);
  EXPECT_GT(sum_with_tiny(), 1.0);
  ASSERT_TRUE(establish_environment(control_).has_value());
  EXPECT_EQ(sum_with_tiny(), 1.0);
}

// F21: the contract says a thread inherits the state of the thread that started
// it, and that this is a property of the platform to be confirmed against the
// real kernel. These two tests are that confirmation.
TEST_F(SubnormalTest, AThreadStartedAfterEstablishingInheritsTheRequiredState) {
  ASSERT_TRUE(establish_environment(control_).has_value());
  bool clean = false;
  std::thread worker([&clean] {
    RealFpControl own;
    clean = assert_environment(own).has_value() &&
            std::bit_cast<std::uint64_t>(half_of_min_normal()) == kHalfMinNormalBits;
  });
  worker.join();
  EXPECT_TRUE(clean);
}

TEST_F(SubnormalTest, AThreadStartedUnderADirtyStateInheritsThatAndItsOwnWritesStayItsOwn) {
  FpState dirty = required();
  dirty.flush_to_zero = true;
  control_.write(dirty);
  bool inherited_dirty = false;
  bool repaired = false;
  std::thread worker([&] {
    RealFpControl own;
    inherited_dirty = !assert_environment(own).has_value();
    repaired = establish_environment(own).has_value();
  });
  worker.join();
  EXPECT_TRUE(inherited_dirty) << "the state is inherited, so threads must assert on start";
  EXPECT_TRUE(repaired);
  EXPECT_TRUE(control_.read().flush_to_zero) << "a thread's write never reaches its parent";
}

}  // namespace
}  // namespace loadforge::fpenv
