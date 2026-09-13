// SPDX-License-Identifier: GPL-3.0-or-later
//
// Result is a template, and gcov counts each instantiation separately: a method
// exercised for Result<Duration, ParseError> is *not* covered for
// Result<int, SyscallError>. That is not a quirk to work around — it is the
// gate doing its job, because a value_or or an error() that is never called for
// a given instantiation is genuinely untested code for that type.
//
// This project has already been caught by exactly that once: value_or's
// fallback went unexercised for two instantiations and the coverage gate
// refused the change. So every Result the platform layer introduces has its
// whole surface driven here, on both alternatives.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {
namespace {

using core::Ok;
using core::Result;

/// Drive every method of one instantiation, on both alternatives.
template <typename T>
void exercise(T ok_value, T fallback) {
  const SyscallError failure{EACCES, "open", "/sys/denied"};

  const Result<T, SyscallError> good(ok_value);
  EXPECT_TRUE(good.has_value());
  EXPECT_TRUE(static_cast<bool>(good));
  EXPECT_EQ(good.value(), ok_value);
  EXPECT_EQ(good.value_or(fallback), ok_value);
  EXPECT_THROW((void)good.error(), std::bad_variant_access);

  const Result<T, SyscallError> bad(failure);
  EXPECT_FALSE(bad.has_value());
  EXPECT_FALSE(static_cast<bool>(bad));
  EXPECT_EQ(bad.error(), failure);
  EXPECT_EQ(bad.value_or(fallback), fallback);
  EXPECT_THROW((void)bad.value(), std::bad_variant_access);
}

TEST(PlatformResults, IntInstantiation) { exercise<int>(7, -1); }

TEST(PlatformResults, SizeInstantiation) { exercise<std::size_t>(4096U, 0U); }

TEST(PlatformResults, StringInstantiation) { exercise<std::string>("95000\n", "fallback"); }

TEST(PlatformResults, OkInstantiation) { exercise<Ok>(Ok{}, Ok{}); }

TEST(PlatformResults, OksAreAllEqual) {
  // Ok carries no information, so any two are the same value. Stated as a
  // test because the comparison is real code that the gate counts.
  EXPECT_EQ(Ok{}, Ok{});
  EXPECT_TRUE(Ok{} == Ok{});
}

}  // namespace
}  // namespace loadforge::platform
