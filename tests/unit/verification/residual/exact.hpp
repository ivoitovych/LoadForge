// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_RESIDUAL_EXACT_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_RESIDUAL_EXACT_HPP

// Exact arithmetic for the residual and dense-linear tests: the truth the
// derivation's premises are checked against, computed without rounding.
//
// Every value the dense-linear specification generates is a multiple of 2^-52
// in [-1, 1) (workloads/dense_linear/spec.hpp), so it is an integer of at most
// 53 bits times 2^-52, a product of two is an integer below 2^104 times
// 2^-104, and a sum of 256 such products is below 2^112: inside a 128-bit
// integer, exactly. These helpers refuse (by test failure) any value that is
// not such a multiple, so no test checks a bound against a rounded "truth".

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::verification::exact {

__extension__ using Int128 = __int128;
__extension__ using UInt128 = unsigned __int128;

/// a * b, exactly: |a|, |b| <= 2^53, so the product is below 2^106 and the
/// wrapped unsigned product, read back as signed, is the true one (C++20
/// defines that conversion as modular). Multiplied unsigned because a signed
/// 128-bit multiplication under -fsanitize=undefined calls __muloti4, which
/// the ARM64 toolchain's runtime does not provide, and the link fails.
inline Int128 product(std::int64_t a, std::int64_t b) {
  return static_cast<Int128>(static_cast<UInt128>(static_cast<Int128>(a)) *
                             static_cast<UInt128>(static_cast<Int128>(b)));
}

/// x as an integer count of 2^-52. x must be such a multiple with |x| < 2^11.
inline std::int64_t scaled52(double x) {
  const double scaled = std::ldexp(x, 52);
  EXPECT_EQ(scaled, std::trunc(scaled)) << x << " is not a multiple of 2^-52";
  return static_cast<std::int64_t>(scaled);
}

/// An exact sum of products and of their magnitudes, both in units of 2^-104.
struct Dot {
  Int128 value = 0;
  Int128 magnitude = 0;
};

inline Dot dot(std::span<const double> x, std::span<const double> y) {
  Dot result;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const Int128 term = product(scaled52(x[i]), scaled52(y[i]));
    result.value += term;
    result.magnitude += term < 0 ? -term : term;
  }
  return result;
}

/// A count of 2^-104 as a double: rounded once, by the conversion, then scaled
/// exactly (every nonzero count is at least 1, so 2^-104 times it is normal).
inline double to_double(Int128 count) { return std::ldexp(static_cast<double>(count), -104); }

/// A double that is a multiple of 2^-104 below 2^23 in magnitude, as a count.
/// Every result of a dot product of spec entries is one (see the header
/// comment: every rounding of a multiple of 2^-104 above 2^-52 is another).
inline Int128 to_count(double x) {
  const double scaled = std::ldexp(x, 104);
  EXPECT_EQ(scaled, std::trunc(scaled)) << x << " is not a multiple of 2^-104";
  return static_cast<Int128>(scaled);
}

inline double magnitude_of(Int128 count) { return to_double(count < 0 ? -count : count); }

}  // namespace loadforge::verification::exact

#endif
