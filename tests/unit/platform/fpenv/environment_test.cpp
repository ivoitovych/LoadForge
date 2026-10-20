// SPDX-License-Identifier: GPL-3.0-or-later
//
// FpEnvironment: establish and assert, with the failure path forced through an
// injected FpControl (M2b P2): a register something else left dirty, and a
// register that does not take the write.
#include "platform/fpenv/environment.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "contracts/fpenv/fpenv_contract.hpp"

namespace loadforge::fpenv {
namespace {

/// A register file in memory. `refuse_writes` models a platform that ignores a
/// write, which the contract says is reported by the read that follows.
class ScriptedControl final : public FpControl {
 public:
  explicit ScriptedControl(FpState initial) : state_(initial) {}

  [[nodiscard]] FpState read() const override { return state_; }
  void write(const FpState& state) override {
    ++writes;
    if (!refuse_writes) {
      state_.rounding = state.rounding;
      state_.flush_to_zero = state.flush_to_zero;
      if (state.denormals_are_zero.has_value()) {
        state_.denormals_are_zero = state.denormals_are_zero;
      }
      if (state.default_nan.has_value()) {
        state_.default_nan = state.default_nan;
      }
    }
  }

  FpState state_;
  bool refuse_writes = false;
  int writes = 0;
};

const FpState kDirtyX86{Rounding::kUpward, true, true, std::nullopt};
const FpState kDirtyArm{Rounding::kDownward, true, std::nullopt, true};

TEST(EnvironmentTest, AnEstablishedStateAssertsClean) {
  ScriptedControl control(kDirtyX86);
  ASSERT_TRUE(establish_environment(control).has_value());
  EXPECT_EQ(control.read(), required_state(true, false));
  EXPECT_TRUE(assert_environment(control).has_value());
}

TEST(EnvironmentTest, EstablishingOnTheOtherShapeOfArchitectureWritesItsOwnFlags) {
  ScriptedControl control(kDirtyArm);
  ASSERT_TRUE(establish_environment(control).has_value());
  EXPECT_EQ(control.read(), required_state(false, true));
}

TEST(EnvironmentTest, AssertingADirtyRegisterNamesEveryFlagAndWritesNothing) {
  const ScriptedControl control(kDirtyX86);
  const auto asserted = assert_environment(control);
  ASSERT_FALSE(asserted.has_value());
  EXPECT_EQ(asserted.error().violations,
            (std::vector<FpViolation>{
                {"rounding", "upward"}, {"flush_to_zero", "on"}, {"denormals_are_zero", "on"}}));
  EXPECT_EQ(control.writes, 0) << "asserting never repairs: it reports";
}

TEST(EnvironmentTest, ARegisterThatRefusesTheWriteIsReportedAsDirty) {
  ScriptedControl control(kDirtyArm);
  control.refuse_writes = true;
  const auto established = establish_environment(control);
  ASSERT_FALSE(established.has_value());
  EXPECT_EQ(control.writes, 1);
  EXPECT_EQ(established.error().violations,
            (std::vector<FpViolation>{
                {"rounding", "downward"}, {"flush_to_zero", "on"}, {"default_nan", "on"}}));
}

TEST(EnvironmentTest, ALibraryThatChangesTheStateAfterStartupIsCaughtAndRepaired) {
  ScriptedControl control(required_state(true, false));
  ASSERT_TRUE(establish_environment(control).has_value());
  control.state_.flush_to_zero = true;  // what a fast-math library does at load time
  EXPECT_FALSE(assert_environment(control).has_value());
  ASSERT_TRUE(establish_environment(control).has_value()) << "and re-establishing repairs it";
  EXPECT_TRUE(assert_environment(control).has_value());
}

TEST(EnvironmentTest, TheMessageNamesEveryViolationOnceAndWhatIsRequired) {
  const FpError error{{{"rounding", "upward"}, {"flush_to_zero", "on"}}};
  const std::string message = describe(error);
  EXPECT_NE(message.find("rounding is upward, flush_to_zero is on"), std::string::npos) << message;
  EXPECT_NE(message.find("required: round to nearest"), std::string::npos) << message;
  const FpError single{{{"default_nan", "on"}}};
  EXPECT_NE(describe(single).find("(default_nan is on)"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::fpenv

namespace loadforge::contracts {
namespace {

// The same suite the real implementation passes, against this file's mock of
// the contract: the mock is a reading of FpControl, and the suite compares it.
struct ScriptedSubject {
  static constexpr bool kHasDenormalsAreZero = true;
  static constexpr bool kHasDefaultNan = false;
  static std::unique_ptr<fpenv::FpControl> make() {
    return std::make_unique<fpenv::ScriptedControl>(fpenv::required_state(true, false));
  }
};
INSTANTIATE_TYPED_TEST_SUITE_P(Scripted, FpControlContract, ::testing::Types<ScriptedSubject>);

}  // namespace
}  // namespace loadforge::contracts
