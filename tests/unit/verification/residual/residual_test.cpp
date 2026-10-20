// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/verification/residual: the oracle's loops, gamma, the domain of
// the derivation with each refusal, the tolerance against the bound (3) of the
// header computed in wider arithmetic, and the comparison at the bound from
// both sides (M2b exit criterion 3). P3: zero, subnormal, NaN and infinite
// inputs.

#include "verification/residual/residual.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::verification::residual {
namespace {

constexpr double kU = kUnitRoundoff;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTiny = 0x1p-1074;

double up(double x) { return std::nextafter(x, kInf); }
double down(double x) { return std::nextafter(x, 0.0); }

/// The derived tolerance; value() throws, failing the test, if there is none.
double derived(ErrorModel model, double magnitude) { return tolerance(model, magnitude).value(); }

TEST(ResidualGamma, IsDepthTimesUOverOneMinusThat) {
  EXPECT_EQ(gamma(0), 0.0);
  EXPECT_EQ(gamma(1), kU / (1.0 - kU));
  EXPECT_EQ(gamma(256), 256.0 * kU / (1.0 - 256.0 * kU));
  // The specification's own gamma, which the derivation's first step is:
  // two readings of one formula, compared.
  EXPECT_EQ(gamma(workloads::dense_linear::kOrder),
            workloads::dense_linear::gamma(workloads::dense_linear::kOrder));
  EXPECT_EQ(kU, workloads::dense_linear::kUnitRoundoff);
  EXPECT_GT(gamma(kMaxDepth), 0x1p-27);
  EXPECT_LT(gamma(kMaxDepth), 0x1p-26);
}

TEST(ResidualLoops, DotIsIncreasingOrderWithMagnitudes) {
  const std::vector<double> x{0.5, -0.25, 0.75};
  const std::vector<double> y{0.5, 0.5, -1.0};
  const Accumulation result = dot(x, y);
  EXPECT_EQ(result.value, 0.25 - 0.125 - 0.75);
  EXPECT_EQ(result.magnitude, 0.25 + 0.125 + 0.75);

  const Accumulation empty = dot({}, {});
  EXPECT_EQ(empty.value, 0.0);
  EXPECT_EQ(empty.magnitude, 0.0);

  // Only the common length is used.
  const std::vector<double> longer{0.5, -0.25, 0.75, 1000.0};
  EXPECT_EQ(dot(longer, y).value, result.value);
  EXPECT_EQ(dot(y, longer).magnitude, result.magnitude);
}

TEST(ResidualLoops, TheOrderIsIncreasingNotAnyOther) {
  // 1 + 2^-53 + 2^-53 is 1 in increasing order (each tiny term is a tie
  // rounded to even) and 1 + 2^-52 in decreasing order.
  const std::vector<double> x{1.0, kU, kU};
  const std::vector<double> ones{1.0, 1.0, 1.0};
  EXPECT_EQ(dot(x, ones).value, 1.0);
  EXPECT_EQ(sum(x).value, 1.0);
  const std::vector<double> reversed{kU, kU, 1.0};
  EXPECT_EQ(sum(reversed).value, 1.0 + 2.0 * kU);
}

TEST(ResidualLoops, SumIsIncreasingOrderWithMagnitudes) {
  const std::vector<double> x{0.5, -0.25, -1.0};
  const Accumulation result = sum(x);
  EXPECT_EQ(result.value, -0.75);
  EXPECT_EQ(result.magnitude, 1.75);
  EXPECT_EQ(sum({}).value, 0.0);
  EXPECT_EQ(sum({}).magnitude, 0.0);
}

TEST(ResidualLoops, NonFiniteValuesReachTheMagnitude) {
  const std::vector<double> nan{0.5, kNaN};
  const std::vector<double> inf{0.5, -kInf};
  const std::vector<double> ones{1.0, 1.0};
  EXPECT_TRUE(std::isnan(dot(nan, ones).magnitude));
  EXPECT_EQ(dot(inf, ones).magnitude, kInf);
  EXPECT_TRUE(std::isnan(sum(nan).magnitude));
  EXPECT_EQ(sum(inf).magnitude, kInf);
  // A product that overflows is an infinite magnitude, not a wrapped one.
  const std::vector<double> big{0x1p600};
  EXPECT_EQ(dot(big, big).magnitude, kInf);
}

TEST(ResidualModels, DotAndSum) {
  EXPECT_EQ(dot_model(256).depth, 256U);
  EXPECT_EQ(dot_model(256).products, 256U);
  EXPECT_EQ(sum_model(256).depth, 255U);
  EXPECT_EQ(sum_model(256).products, 0U);
  EXPECT_EQ(sum_model(1).depth, 0U);
  EXPECT_EQ(sum_model(0).depth, 0U);
}

TEST(ResidualDomain, EachRefusalAlone) {
  EXPECT_EQ(tolerance({kMaxDepth + 1, 0}, 1.0).error(), Underivable::kDepthTooLarge);
  EXPECT_EQ(tolerance({0, kMaxDepth + 1}, 1.0).error(), Underivable::kDepthTooLarge);
  EXPECT_EQ(tolerance({256, 256}, kNaN).error(), Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(tolerance({256, 256}, kInf).error(), Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(tolerance({256, 256}, -kInf).error(), Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(tolerance({256, 256}, -kTiny).error(), Underivable::kNegativeMagnitude);
  EXPECT_EQ(tolerance({256, 256}, up(kMaxMagnitude)).error(), Underivable::kMagnitudeTooLarge);
  EXPECT_EQ(tolerance({256, 256}, std::numeric_limits<double>::max()).error(),
            Underivable::kMagnitudeTooLarge);
}

TEST(ResidualDomain, ItsEdgesAreInside) {
  for (const ErrorModel model : {ErrorModel{kMaxDepth, kMaxDepth}, ErrorModel{0, 0}}) {
    for (const double magnitude : {0.0, -0.0, kTiny, 1.0, kMaxMagnitude}) {
      const double t = derived(model, magnitude);
      EXPECT_TRUE(std::isfinite(t)) << magnitude;
      EXPECT_GT(t, 0.0) << magnitude;
    }
  }
}

TEST(ResidualDomain, ReasonsAreNamed) {
  EXPECT_EQ(describe(Underivable::kNonFiniteMagnitude), "the magnitude sum is not finite");
  EXPECT_EQ(describe(Underivable::kNegativeMagnitude), "the magnitude sum is negative");
  EXPECT_EQ(describe(Underivable::kMagnitudeTooLarge), "the magnitude sum exceeds 2^1000");
  EXPECT_EQ(describe(Underivable::kDepthTooLarge), "the operation count exceeds 2^26");
  EXPECT_EQ(describe(static_cast<Underivable>(9)), "unrecognised reason");
}

TEST(ResidualTolerance, WithNothingToRoundItIsTheUnderflowTermAlone) {
  // S^ = 0: no term has any magnitude, so only V remains, exactly.
  EXPECT_EQ(derived(dot_model(256), 0.0), (4.0 * 256.0 + 1.0) * kTiny);
  EXPECT_EQ(derived(sum_model(256), 0.0), kTiny);
  EXPECT_EQ(derived({0, 0}, 0.0), kTiny);
}

/// (3) of the header, B = (1 + u)(G S^ + (G + 2) U), in long double: wider
/// than double on both architectures the project builds for (64-bit and
/// 113-bit significands), so B's few-ulp margins are resolved.
long double bound_b(ErrorModel model, double magnitude) {
  const long double u = kU;
  const auto k = static_cast<long double>(model.depth);
  const long double g = k * u / (1.0L - k * u);
  const long double big_g = 2.0L * g / (1.0L - g);
  const long double unit = static_cast<long double>(model.products) * 0x1p-1074L;
  return (1.0L + u) * (big_g * magnitude + (big_g + 2.0L) * unit);
}

TEST(ResidualTolerance, IsNeverBelowTheBoundAndBarelyAboveIt) {
  static_assert(LDBL_MANT_DIG >= 64, "the check needs a wider significand than double's");
  // Depths and magnitudes across the domain, and random magnitudes, so that
  // a rounding the derivation forgot -- an up-step too few, a rounding of K
  // assumed exact -- shows as T^ < B for some of them.
  const std::vector<ErrorModel> models{
      dot_model(1),   dot_model(2),   dot_model(3), dot_model(256),     dot_model(255),
      dot_model(257), sum_model(256), sum_model(2), dot_model(1000003), {kMaxDepth, kMaxDepth}};
  std::vector<double> magnitudes{1.0, 0.5, 3.0, 255.75, 256.0, 1e-300, 1e300, kMaxMagnitude};
  for (std::uint64_t draw = 0; draw < 2000; ++draw) {
    const std::uint64_t bits = rng::u64_of(rng::block_at(7, 0, draw));
    magnitudes.push_back(
        std::ldexp(1.0 + rng::unit_interval(bits), static_cast<int>(draw % 200) - 100));
  }
  long double worst = 0.0L;
  for (const ErrorModel& model : models) {
    for (const double magnitude : magnitudes) {
      const long double b = bound_b(model, magnitude);
      const long double t = derived(model, magnitude);
      ASSERT_GE(t, b) << model.depth << " " << magnitude;
      // No looser than the 24 u the header admits, plus V, plus one subnormal
      // step where K S^ itself lands among the subnormals and rounds up.
      const long double v = (4.0L * static_cast<long double>(model.products) + 2.0L) * 0x1p-1074L;
      EXPECT_LE(t, b * (1.0L + 24.0L * kU) + v) << model.depth << " " << magnitude;
      if (model.depth > 0 && magnitude >= 1e-250) {
        worst = std::max(worst, t / b - 1.0L);
      }
    }
  }
  // The slack actually spent, in units of u, printed for the journal.
  std::cout << "tolerance / B - 1, at most: " << static_cast<double>(worst / kU) << " u\n";
}

TEST(ResidualTolerance, CoversAnEvaluationThatUnderflowed) {
  // Each product 2^-1074 * 0.5 = 2^-1075 is a tie between 0 and 2^-1074 and
  // rounds to 0, so a correct evaluation gives 0 where the exact value is
  // n 2^-1075. Only the underflow term U covers that: the magnitude sum is
  // itself 0, and a tolerance of gamma S^ alone would be zero.
  const std::size_t n = 256;
  const std::vector<double> x(n, kTiny);
  const std::vector<double> y(n, 0.5);
  const Accumulation computed = dot(x, y);
  EXPECT_EQ(computed.value, 0.0);
  EXPECT_EQ(computed.magnitude, 0.0);
  const double exact = static_cast<double>(n) / 2.0 * kTiny;  // n 2^-1075, n even
  const double t = derived(dot_model(n), computed.magnitude);
  EXPECT_TRUE(within(compare(exact, computed.value, t)));
  // And the underflow term is what holds it: the floor alone would refuse.
  EXPECT_FALSE(within(compare(exact, computed.value, kTiny)));
}

TEST(ResidualTolerance, CoversSubnormalInputsThatDoNotUnderflow) {
  // A subnormal times 2^-10 is exact in the subnormal range; any order agrees.
  const std::vector<double> x{0x1p-1050, -0x1p-1060, 0x1p-1040};
  const std::vector<double> y{0x1p-10, 0x1p-10, 0x1p-10};
  const Accumulation computed = dot(x, y);
  EXPECT_EQ(computed.value, 0x1p-1060 - 0x1p-1070 + 0x1p-1050);
  const double t = derived(dot_model(3), computed.magnitude);
  EXPECT_TRUE(within(compare(computed.value, computed.value, t)));
  EXPECT_GT(t, 0.0);
}

TEST(ResidualCompare, ResidualIsTheAbsoluteDifference) {
  EXPECT_EQ(compare(1.5, 1.0, 0.25).residual, 0.5);
  EXPECT_EQ(compare(1.0, 1.5, 0.25).residual, 0.5);
  EXPECT_EQ(compare(1.0, 1.5, 0.25).tolerance, 0.25);
  EXPECT_EQ(compare(-0.0, 0.0, 0.25).residual, 0.0);
}

TEST(ResidualCompare, AtTheBoundFromBothSides) {
  // The comparison at the bound itself: equal is held, one representable step
  // above is breached, one below is held.
  const double t = derived(dot_model(256), 37.5);
  EXPECT_TRUE(within(Comparison{t, t}));
  EXPECT_FALSE(within(Comparison{up(t), t}));
  EXPECT_TRUE(within(Comparison{down(t), t}));
}

TEST(ResidualCompare, BreachedAtOnePlusEpsilonHeldAtOneMinusEpsilon) {
  // M2b exit criterion 3, at the residual level. First the residual itself at
  // (1 +/- eps) T^ for the derived T^, with eps machine epsilon -- the
  // smallest relative step the claim can be made at -- and a wider 2^-20.
  const double reference = 32.0;
  const double t = derived(dot_model(256), 32.0);
  for (const double eps : {DBL_EPSILON, 0x1p-20}) {
    EXPECT_FALSE(within(Comparison{t * (1.0 + eps), t})) << eps;
    EXPECT_TRUE(within(Comparison{t * (1.0 - eps), t})) << eps;
  }
  // Then through compare(), from a kernel value c' + (1 +/- eps) T^ around an
  // exact oracle value c' = 32. The sum rounds to a double near 32, so the
  // residual is (1 +/- eps) T^ only to within half an ulp of 32 (2^-48), and
  // eps must make eps T^ (T^ is about 2^-39) wider than that: 1/16 is 2^-43,
  // sixteen ulps, unambiguous.
  const double eps = 1.0 / 16.0;
  EXPECT_GT(t * eps, 0x1p-48);
  EXPECT_FALSE(within(compare(reference + t * (1.0 + eps), reference, t)));
  EXPECT_TRUE(within(compare(reference + t * (1.0 - eps), reference, t)));
  EXPECT_FALSE(within(compare(reference - t * (1.0 + eps), reference, t)));
  EXPECT_TRUE(within(compare(reference - t * (1.0 - eps), reference, t)));
}

TEST(ResidualCompare, NaNAndInfinityAreNeverWithin) {
  const double t = derived(dot_model(256), 1.0);
  EXPECT_FALSE(within(compare(kNaN, 0.5, t)));
  EXPECT_FALSE(within(compare(0.5, kNaN, t)));
  EXPECT_FALSE(within(compare(kInf, 0.5, t)));
  EXPECT_FALSE(within(compare(-kInf, 0.5, t)));
  EXPECT_FALSE(within(compare(kInf, kInf, t))) << "inf - inf is NaN, a failure";
  // A difference that overflows is infinite, and breached.
  EXPECT_EQ(
      compare(std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), t).residual,
      kInf);
}

TEST(ResidualCompare, ZeroInputsHaveAPositiveToleranceAndNoMore) {
  const double t = derived(dot_model(256), 0.0);
  EXPECT_TRUE(within(compare(0.0, 0.0, t)));
  EXPECT_TRUE(within(compare(-0.0, 0.0, t)));
  EXPECT_TRUE(within(compare(t, 0.0, t)));
  EXPECT_FALSE(within(compare(up(t), 0.0, t)));
}

}  // namespace
}  // namespace loadforge::verification::residual
