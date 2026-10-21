// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_FPENV_FP_CONTROL_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_FPENV_FP_CONTROL_CONTRACT_HPP

// Provider-side suite for fpenv::FpControl in src/platform/fpenv/fpenv.hpp
// (ADR 0003, 0004).
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

using fpenv::FpState;
using fpenv::Rounding;

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
  const FpState partial{};  // names rounding and flush_to_zero only
  this->control_->write(partial);
  const FpState after = this->control_->read();
  EXPECT_EQ(after.denormals_are_zero, dirty.denormals_are_zero);
  EXPECT_EQ(after.default_nan, dirty.default_nan);
}

REGISTER_TYPED_TEST_SUITE_P(FpControlContract, ReadReportsExactlyTheFlagsTheArchitectureHas,
                            WhatIsWrittenIsWhatIsReadBack, AnAbsentPropertyIsLeftAsItIs);

}  // namespace loadforge::contracts

#endif
