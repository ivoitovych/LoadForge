// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/parse_error.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string_view>

namespace loadforge::core {
namespace {

constexpr ParseError kAll[] = {
    ParseError::Empty,       ParseError::InvalidNumber, ParseError::MissingUnit,
    ParseError::UnknownUnit, ParseError::NegativeValue, ParseError::OutOfRange,
};

TEST(ParseError, EveryValueHasNonEmptyDescription) {
  for (const ParseError error : kAll) {
    EXPECT_FALSE(describe(error).empty()) << "value " << static_cast<int>(error);
  }
}

TEST(ParseError, DescriptionsAreDistinct) {
  // A shared string would make two different configuration mistakes read
  // identically to the user, which defeats the point of a specific enum.
  std::set<std::string_view> seen;
  for (const ParseError error : kAll) {
    EXPECT_TRUE(seen.insert(describe(error)).second)
        << "duplicate text for value " << static_cast<int>(error);
  }
}

TEST(ParseError, IsTotalForValuesOutsideTheEnumerators) {
  // The underlying type is uint8_t, so 200 is representable and this cast is
  // well defined even though it names no enumerator. (999 would NOT be
  // representable and the cast itself would be undefined -- the kind of detail
  // that turns a defensive test into the bug it was guarding against.)
  //
  // describe() must stay total: if someone adds an enumerator and forgets this
  // function, the result should be a usable string, not a fall off the end of a
  // non-void function.
  EXPECT_EQ(describe(static_cast<ParseError>(200)), "unrecognised parse error");
}

}  // namespace
}  // namespace loadforge::core
