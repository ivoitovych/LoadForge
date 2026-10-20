// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_FPENV_FPENV_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_FPENV_FPENV_CONTRACT_HPP

// Provider-side suites for src/platform/fpenv/fpenv.hpp (ADR 0003, 0004).
//
// FpCheckContract<Subject>: Subject::check has the signature of fpenv::check.
//
// FpControlContract<Subject>: Subject::make() returns a std::unique_ptr to an
// FpControl; Subject::kHasDenormalsAreZero and Subject::kHasDefaultNan say
// which flags the architecture it controls has. The suite changes the calling
// thread's state and restores what it found, so it may run against the real
// registers.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "platform/fpenv/fpenv.hpp"

namespace loadforge::contracts {

template <typename Subject>
class FpCheckContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(FpCheckContract);

using fpenv::FpState;
using fpenv::FpViolation;
using fpenv::Rounding;

TYPED_TEST_P(FpCheckContract, TheRequiredStateConformsOnEitherArchitecture) {
  EXPECT_TRUE(TypeParam::check(fpenv::required_state(true, false)).has_value());
  EXPECT_TRUE(TypeParam::check(fpenv::required_state(false, true)).has_value());
  EXPECT_TRUE(TypeParam::check(FpState{}).has_value()) << "absent flags are never violations";
}

TYPED_TEST_P(FpCheckContract, EachDepartureIsNamedWithWhatWasFound) {
  for (const Rounding rounding : {Rounding::kTowardZero, Rounding::kUpward, Rounding::kDownward}) {
    FpState state = fpenv::required_state(true, false);
    state.rounding = rounding;
    const auto checked = TypeParam::check(state);
    ASSERT_FALSE(checked.has_value());
    ASSERT_EQ(checked.error().violations.size(), 1U);
    EXPECT_EQ(checked.error().violations[0],
              (FpViolation{"rounding", std::string(fpenv::describe(rounding))}));
  }
  FpState ftz;
  ftz.flush_to_zero = true;
  EXPECT_EQ(TypeParam::check(ftz).error().violations,
            (std::vector<FpViolation>{{"flush_to_zero", "on"}}));
  FpState daz;
  daz.denormals_are_zero = true;
  EXPECT_EQ(TypeParam::check(daz).error().violations,
            (std::vector<FpViolation>{{"denormals_are_zero", "on"}}));
  FpState dn;
  dn.default_nan = true;
  EXPECT_EQ(TypeParam::check(dn).error().violations,
            (std::vector<FpViolation>{{"default_nan", "on"}}));
}

TYPED_TEST_P(FpCheckContract, EveryViolationIsReportedInTheFixedOrder) {
  const FpState everything_wrong{Rounding::kUpward, true, true, true};
  const auto checked = TypeParam::check(everything_wrong);
  ASSERT_FALSE(checked.has_value());
  EXPECT_EQ(checked.error().violations, (std::vector<FpViolation>{{"rounding", "upward"},
                                                                  {"flush_to_zero", "on"},
                                                                  {"denormals_are_zero", "on"},
                                                                  {"default_nan", "on"}}));
}

REGISTER_TYPED_TEST_SUITE_P(FpCheckContract, TheRequiredStateConformsOnEitherArchitecture,
                            EachDepartureIsNamedWithWhatWasFound,
                            EveryViolationIsReportedInTheFixedOrder);

template <typename Subject>
class FpControlContract : public ::testing::Test {
 protected:
  void SetUp() override {
    control_ = Subject::make();
    found_ = control_->read();
  }
  void TearDown() override { control_->write(found_); }

  [[nodiscard]] FpState required() const {
    return fpenv::required_state(Subject::kHasDenormalsAreZero, Subject::kHasDefaultNan);
  }

  std::unique_ptr<fpenv::FpControl> control_;
  FpState found_;
};
TYPED_TEST_SUITE_P(FpControlContract);

TYPED_TEST_P(FpControlContract, ReadReportsExactlyTheFlagsTheArchitectureHas) {
  const FpState state = this->control_->read();
  EXPECT_EQ(state.denormals_are_zero.has_value(), TypeParam::kHasDenormalsAreZero);
  EXPECT_EQ(state.default_nan.has_value(), TypeParam::kHasDefaultNan);
}

TYPED_TEST_P(FpControlContract, WhatIsWrittenIsWhatIsReadBack) {
  this->control_->write(this->required());
  EXPECT_EQ(this->control_->read(), this->required());

  for (const Rounding rounding :
       {Rounding::kTowardZero, Rounding::kUpward, Rounding::kDownward, Rounding::kToNearestEven}) {
    FpState state = this->required();
    state.rounding = rounding;
    this->control_->write(state);
    EXPECT_EQ(this->control_->read(), state) << fpenv::describe(rounding);
  }

  FpState flushed = this->required();
  flushed.flush_to_zero = true;
  this->control_->write(flushed);
  EXPECT_EQ(this->control_->read(), flushed);
  this->control_->write(this->required());
  EXPECT_EQ(this->control_->read(), this->required()) << "and the flag can be cleared again";
}

TYPED_TEST_P(FpControlContract, AnAbsentPropertyIsLeftAsItIs) {
  FpState dirty = this->required();
  if (TypeParam::kHasDenormalsAreZero) {
    dirty.denormals_are_zero = true;
  }
  if (TypeParam::kHasDefaultNan) {
    dirty.default_nan = true;
  }
  this->control_->write(dirty);
  FpState partial;  // names rounding and flush_to_zero only
  this->control_->write(partial);
  const FpState after = this->control_->read();
  EXPECT_EQ(after.denormals_are_zero, dirty.denormals_are_zero);
  EXPECT_EQ(after.default_nan, dirty.default_nan);
}

REGISTER_TYPED_TEST_SUITE_P(FpControlContract, ReadReportsExactlyTheFlagsTheArchitectureHas,
                            WhatIsWrittenIsWhatIsReadBack, AnAbsentPropertyIsLeftAsItIs);

}  // namespace loadforge::contracts

#endif
