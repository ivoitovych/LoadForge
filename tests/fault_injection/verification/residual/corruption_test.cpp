// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for src/verification/residual.
//
// T5: a correct dot product from the specification's inputs, every one of its
// 64 bits flipped in turn. The comparison must breach exactly when the flip
// moved the value by more than the tolerance allows, and must breach for
// every flip the bound has no business hiding: the sign, every exponent bit,
// and every significand bit worth more than twice the tolerance. What it
// cannot see -- a flip of a low significand bit, worth less than the rounding
// a correct kernel may itself commit -- is stated, counted, and asserted to be
// only those bits: a bounded check is blind below its bound by construction,
// and saying so is the honest form of that limit (verification.md §5).
//
// T5b: values from which no tolerance can be derived -- a NaN, an infinity, a
// product that overflows, more terms than the derivation covers -- give the
// Underivable arm, never a tolerance; the record such a check becomes is
// VerificationNotPerformed, never a pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/verification/residual/orders.hpp"
#include "verification/record.hpp"
#include "verification/residual/residual.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::verification::residual {
namespace {

namespace spec = workloads::dense_linear;
constexpr std::size_t kN = spec::kOrder;

std::vector<double> entries(std::uint64_t stream, std::uint64_t position) {
  std::vector<double> out(kN);
  for (std::size_t k = 0; k < kN; ++k) {
    out[k] = spec::entry_value(rng::u64_of(rng::block_at(99, stream, position + k)));
  }
  return out;
}

double flip(double value, int bit) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << bit));
}

TEST(ResidualCorruption, EveryBitFlipOfAResultIsCaughtUnlessTheBoundCoversIt) {
  int blind_bits_most = 0;
  for (std::uint64_t unit = 0; unit < 16; ++unit) {
    const std::vector<double> x = entries(unit, 0);
    const std::vector<double> y = entries(unit, kN);
    const Accumulation oracle = dot(x, y);
    const double t = tolerance(dot_model(kN), oracle.magnitude).value();
    // The kernel's value: a four-lane fused panel order, correct by T3.
    const double kernel = orders::evaluate(orders::Order{orders::Shape::kPanel, 64, 4, true}, x, y);
    const double honest = compare(kernel, oracle.value, t).residual;
    ASSERT_LE(honest, t);
    int blind = 0;
    for (int bit = 0; bit < 64; ++bit) {
      const double corrupt = flip(kernel, bit);
      const Comparison comparison = compare(corrupt, oracle.value, t);
      const double moved = std::abs(corrupt - kernel);
      // The comparison is sharp: a flip that moved the value further than the
      // bound plus the honest residual is caught, one that moved it less than
      // the bound minus that residual is not, and the sign and exponent bits
      // are always caught.
      if (bit >= 52) {
        EXPECT_FALSE(within(comparison)) << "unit " << unit << ": exponent or sign bit " << bit;
      } else if (moved > t + honest) {
        EXPECT_FALSE(within(comparison)) << "unit " << unit << ": significand bit " << bit;
      } else if (moved < t - honest) {
        EXPECT_TRUE(within(comparison)) << "unit " << unit << ": significand bit " << bit;
      }
      if (within(comparison)) {
        ++blind;
      }
    }
    blind_bits_most = std::max(blind_bits_most, blind);
  }
  // How many depends on |c| against S^: T^ is about 2^-44 S^, and a result
  // that cancels to a small |c| has a small ulp, so more of its low bits sit
  // below the bound. The sharpness above is the claim; the count is printed.
  std::cout << "low significand bits a flip of which the bound covers, at most: " << blind_bits_most
            << " of 52\n";
}

TEST(ResidualCorruption, ANaNOrAnInfinityInTheResultIsCaught) {
  const std::vector<double> x = entries(1, 0);
  const std::vector<double> y = entries(1, kN);
  const Accumulation oracle = dot(x, y);
  const double t = tolerance(dot_model(kN), oracle.magnitude).value();
  for (const double corrupt :
       {std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::signaling_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    const Comparison comparison = compare(corrupt, oracle.value, t);
    EXPECT_FALSE(within(comparison)) << corrupt;
    EXPECT_FALSE(comparison.residual <= comparison.tolerance) << corrupt;
  }
}

TEST(ResidualNotPerformed, NoToleranceIsDerivedFromValuesThatHaveNone) {
  const std::vector<double> ones(kN, 1.0);
  std::vector<double> nan = entries(2, 0);
  nan[17] = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> inf = entries(2, 0);
  inf[200] = -std::numeric_limits<double>::infinity();
  std::vector<double> overflow = entries(2, 0);
  overflow[3] = 0x1p1023;
  const std::vector<double> big(kN, 0x1p505);  // squares 2^1010, summed 2^1018
  struct Case {
    std::string name;
    core::Result<double, Underivable> result;
    Underivable expected;
  };
  const std::vector<Case> cases{
      {"NaN", tolerance(dot_model(kN), dot(nan, ones).magnitude), Underivable::kNonFiniteMagnitude},
      {"infinity", tolerance(dot_model(kN), dot(inf, ones).magnitude),
       Underivable::kNonFiniteMagnitude},
      {"overflowing product", tolerance(dot_model(kN), dot(overflow, big).magnitude),
       Underivable::kNonFiniteMagnitude},
      {"magnitude past 2^1000", tolerance(dot_model(kN), dot(big, big).magnitude),
       Underivable::kMagnitudeTooLarge},
      {"too many terms", tolerance(dot_model(kMaxDepth + 1), 1.0), Underivable::kDepthTooLarge},
  };
  for (const Case& entry : cases) {
    ASSERT_FALSE(entry.result.has_value()) << entry.name;
    EXPECT_EQ(entry.result.error(), entry.expected) << entry.name;
    // What the check becomes: a check that was not made, with zero compared.
    const ErrorRecord record =
        VerificationNotPerformed{Provenance{}, "dot product against its derived bound",
                                 std::string(describe(entry.result.error())), 0};
    EXPECT_EQ(kind_name(record), "VerificationNotPerformed") << entry.name;
    EXPECT_FALSE(is_wrong_result(record)) << entry.name;
    EXPECT_TRUE(is_valid(record)) << entry.name;
  }
}

}  // namespace
}  // namespace loadforge::verification::residual
