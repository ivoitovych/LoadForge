// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference implementation of the floating-point environment
// contract -- an in-memory register file and a check -- run against its
// suites, and the definitions the header carries.

#include <memory>
#include <string>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/fpenv/fpenv_contract.hpp"
#include "platform/fpenv/fpenv.hpp"

namespace loadforge::contracts {
namespace {

using fpenv::FpError;
using fpenv::FpState;
using fpenv::FpViolation;

core::Result<core::Ok, FpError> reference_check(const FpState& state) {
  std::vector<FpViolation> violations;
  if (state.rounding != fpenv::Rounding::kToNearestEven) {
    violations.push_back({"rounding", std::string(fpenv::describe(state.rounding))});
  }
  if (state.flush_to_zero) {
    violations.push_back({"flush_to_zero", "on"});
  }
  if (state.denormals_are_zero.value_or(false)) {
    violations.push_back({"denormals_are_zero", "on"});
  }
  if (state.default_nan.value_or(false)) {
    violations.push_back({"default_nan", "on"});
  }
  if (violations.empty()) {
    return core::Ok{};
  }
  return FpError{violations};
}

struct ReferenceCheck {
  static core::Result<core::Ok, FpError> check(const FpState& state) {
    return reference_check(state);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, FpCheckContract, ::testing::Types<ReferenceCheck>);

/// Registers held in memory, shaped like one architecture or the other.
template <bool kDaz, bool kDn>
class MemoryControl final : public fpenv::FpControl {
 public:
  MemoryControl() : state_(fpenv::required_state(kDaz, kDn)) {}
  [[nodiscard]] FpState read() const override { return state_; }
  void write(const FpState& state) override {
    state_.rounding = state.rounding;
    state_.flush_to_zero = state.flush_to_zero;
    if (kDaz && state.denormals_are_zero.has_value()) {
      state_.denormals_are_zero = state.denormals_are_zero;
    }
    if (kDn && state.default_nan.has_value()) {
      state_.default_nan = state.default_nan;
    }
  }

 private:
  FpState state_;
};

struct X86ShapedControl {
  static constexpr bool kHasDenormalsAreZero = true;
  static constexpr bool kHasDefaultNan = false;
  static std::unique_ptr<fpenv::FpControl> make() {
    return std::make_unique<MemoryControl<true, false>>();
  }
};

struct ArmShapedControl {
  static constexpr bool kHasDenormalsAreZero = false;
  static constexpr bool kHasDefaultNan = true;
  static std::unique_ptr<fpenv::FpControl> make() {
    return std::make_unique<MemoryControl<false, true>>();
  }
};

using ReferenceControls = ::testing::Types<X86ShapedControl, ArmShapedControl>;
INSTANTIATE_TYPED_TEST_SUITE_P(Reference, FpControlContract, ReferenceControls);

TEST(FpEnvDefinition, RoundingNames) {
  EXPECT_EQ(fpenv::describe(fpenv::Rounding::kToNearestEven), "to nearest, ties to even");
  EXPECT_EQ(fpenv::describe(fpenv::Rounding::kTowardZero), "toward zero");
  EXPECT_EQ(fpenv::describe(fpenv::Rounding::kUpward), "upward");
  EXPECT_EQ(fpenv::describe(fpenv::Rounding::kDownward), "downward");
  EXPECT_EQ(fpenv::describe(static_cast<fpenv::Rounding>(200)), "unrecognised rounding");
}

TEST(FpEnvDefinition, TheRequiredStateHasExactlyTheArchitecturesFlagsAllOff) {
  const FpState x86 = fpenv::required_state(true, false);
  EXPECT_EQ(x86, (FpState{fpenv::Rounding::kToNearestEven, false, false, std::nullopt}));
  const FpState arm = fpenv::required_state(false, true);
  EXPECT_EQ(arm, (FpState{fpenv::Rounding::kToNearestEven, false, std::nullopt, false}));
}

TEST(FpEnvEquality, EveryMemberIsCompared) {
  expect_memberwise_equality(
      FpState{fpenv::Rounding::kToNearestEven, false, false, false},
      {[](FpState& s) { s.rounding = fpenv::Rounding::kUpward; },
       [](FpState& s) { s.flush_to_zero = true; }, [](FpState& s) { s.denormals_are_zero.reset(); },
       [](FpState& s) { s.default_nan = true; }});
  expect_memberwise_equality(FpViolation{"rounding", "upward"},
                             {[](FpViolation& v) { v.property = "flush_to_zero"; },
                              [](FpViolation& v) { v.found = "downward"; }});
  expect_memberwise_equality(FpError{{{"rounding", "upward"}}},
                             {[](FpError& e) { e.violations.push_back({"default_nan", "on"}); }});
}

}  // namespace
}  // namespace loadforge::contracts
