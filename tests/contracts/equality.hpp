// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_EQUALITY_HPP
#define LOADFORGE_TESTS_CONTRACTS_EQUALITY_HPP

// A contract's value types compare member by member (operator== = default).
// Each member is a decision in that comparison, and a frozen header's
// decisions are the contract's to test, not the first stream's that happens to
// compare two values: this checks that equal values compare equal and that a
// change to any one member, alone, makes them unequal.

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <type_traits>

namespace loadforge::contracts {

template <typename T>
void expect_memberwise_equality(
    const T& base, std::initializer_list<std::function<void(std::type_identity_t<T>&)>> changes) {
  const T copy = base;
  EXPECT_TRUE(copy == base);
  EXPECT_FALSE(copy != base);
  std::size_t member = 0;
  for (const auto& change : changes) {
    T other = base;
    change(other);
    EXPECT_FALSE(other == base) << "a change to member " << member << " alone is noticed";
    EXPECT_TRUE(other != base) << "member " << member;
    ++member;
  }
}

}  // namespace loadforge::contracts

#endif
