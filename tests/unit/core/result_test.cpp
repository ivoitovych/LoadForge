// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/result.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "core/parse_error.hpp"

namespace loadforge::core {
namespace {

using IntResult = Result<int, ParseError>;

TEST(Result, HoldsAValue) {
  const IntResult r{42};
  EXPECT_TRUE(r.has_value());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r.value(), 42);
}

TEST(Result, HoldsAnError) {
  const IntResult r{ParseError::UnknownUnit};
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error(), ParseError::UnknownUnit);
}

TEST(Result, ValueOrReturnsValueWhenPresent) {
  const IntResult r{7};
  EXPECT_EQ(r.value_or(99), 7);
}

TEST(Result, ValueOrReturnsFallbackWhenAbsent) {
  const IntResult r{ParseError::Empty};
  EXPECT_EQ(r.value_or(99), 99);
}

TEST(Result, WorksWithNonTrivialValueTypes) {
  const Result<std::string, ParseError> ok{std::string{"held"}};
  EXPECT_EQ(ok.value(), "held");
  const Result<std::string, ParseError> bad{ParseError::InvalidNumber};
  EXPECT_EQ(bad.value_or("fallback"), "fallback");
}

TEST(Result, MisuseThrowsRatherThanInvokingUndefinedBehaviour) {
  // The project refuses to rely on undefined behaviour anywhere (design draft
  // §40). An assert would disappear under NDEBUG and leave a null dereference.
  const IntResult err{ParseError::Empty};
  EXPECT_THROW((void)err.value(), std::bad_variant_access);
  const IntResult ok{1};
  EXPECT_THROW((void)ok.error(), std::bad_variant_access);
}

TEST(Result, IsUsableInConstantExpressions) {
  constexpr IntResult r{5};
  static_assert(r.has_value());
  static_assert(r.value() == 5);
  SUCCEED();
}

}  // namespace
}  // namespace loadforge::core
