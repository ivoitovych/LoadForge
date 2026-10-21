// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_FPENV_FP_CHECK_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_FPENV_FP_CHECK_CONTRACT_HPP

// Provider-side suite for fpenv::check in src/platform/fpenv/fpenv.hpp
// (ADR 0003, 0004).
//
// FpCheckContract<Subject>: Subject::check has the signature of fpenv::check.

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

}  // namespace loadforge::contracts

#endif
