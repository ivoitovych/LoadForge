// SPDX-License-Identifier: GPL-3.0-or-later
//
// check() against the contract suite, and the cases the suite leaves to the
// provider: each flag alone, in combination, and an absent flag.
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "contracts/fpenv/fpenv_contract.hpp"
#include "platform/fpenv/fpenv.hpp"

namespace loadforge::fpenv {
namespace {

TEST(CheckTest, EachFlagAloneIsOneViolation) {
  for (const bool daz : {false, true}) {
    for (const bool dn : {false, true}) {
      for (const bool ftz : {false, true}) {
        const FpState state{Rounding::kToNearestEven, ftz, daz, dn};
        const auto checked = check(state);
        const std::size_t expected = (ftz ? 1U : 0U) + (daz ? 1U : 0U) + (dn ? 1U : 0U);
        EXPECT_EQ(checked.has_value(), expected == 0U);
        if (!checked.has_value()) {
          EXPECT_EQ(checked.error().violations.size(), expected);
        }
      }
    }
  }
}

TEST(CheckTest, AnAbsentFlagIsNotAViolationWhileAPresentOneIs) {
  FpState state;
  state.flush_to_zero = true;
  state.denormals_are_zero = std::nullopt;
  state.default_nan = false;
  EXPECT_EQ(check(state).error().violations, (std::vector<FpViolation>{{"flush_to_zero", "on"}}));
}

}  // namespace
}  // namespace loadforge::fpenv

namespace loadforge::contracts {
namespace {

struct RealCheck {
  static core::Result<core::Ok, fpenv::FpError> check(const fpenv::FpState& state) {
    return fpenv::check(state);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Real, FpCheckContract, ::testing::Types<RealCheck>);

}  // namespace
}  // namespace loadforge::contracts
