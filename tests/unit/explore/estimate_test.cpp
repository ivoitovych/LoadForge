// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/estimate.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace loadforge::explore {
namespace {

TEST(TCritical, TableEntriesAreStudentsT) {
  EXPECT_DOUBLE_EQ(t_critical(1), 12.706);
  EXPECT_DOUBLE_EQ(t_critical(2), 4.303);
  EXPECT_DOUBLE_EQ(t_critical(7), 2.365);
  EXPECT_DOUBLE_EQ(t_critical(30), 2.042);
}

TEST(TCritical, ZeroDegreesIsTreatedAsOne) { EXPECT_DOUBLE_EQ(t_critical(0), 12.706); }

TEST(TCritical, BetweenEntriesTheLargerValueIsUsed) {
  EXPECT_DOUBLE_EQ(t_critical(31), 2.042);
  EXPECT_DOUBLE_EQ(t_critical(39), 2.042);
  EXPECT_DOUBLE_EQ(t_critical(40), 2.021);
  EXPECT_DOUBLE_EQ(t_critical(59), 2.021);
  EXPECT_DOUBLE_EQ(t_critical(60), 2.000);
  EXPECT_DOUBLE_EQ(t_critical(119), 2.000);
  EXPECT_DOUBLE_EQ(t_critical(120), 1.980);
  EXPECT_DOUBLE_EQ(t_critical(100000), 1.980);
}

TEST(TCritical, NeverIncreasesWithMoreDegrees) {
  double previous = t_critical(1);
  for (std::size_t degrees = 2; degrees < 400; ++degrees) {
    EXPECT_LE(t_critical(degrees), previous) << degrees;
    previous = t_critical(degrees);
  }
}

TEST(EstimateOf, NoSamplesIsAnEmptyEstimate) {
  const Estimate e = estimate({});
  EXPECT_EQ(e.replicates, 0U);
  EXPECT_EQ(e.mean, 0.0);
  EXPECT_FALSE(e.stddev.has_value());
  EXPECT_FALSE(e.half_width.has_value());
}

TEST(EstimateOf, OneSampleHasAMeanAndNoSpread) {
  const std::vector<double> samples = {41.5};
  const Estimate e = estimate(samples);
  EXPECT_EQ(e.replicates, 1U);
  EXPECT_EQ(e.mean, 41.5);
  EXPECT_FALSE(e.stddev.has_value());
  EXPECT_FALSE(e.half_width.has_value());
}

TEST(EstimateOf, TwoSamplesAreTheSmallestWithAnInterval) {
  const std::vector<double> samples = {10.0, 14.0};
  const Estimate e = estimate(samples);
  EXPECT_EQ(e.replicates, 2U);
  EXPECT_DOUBLE_EQ(e.mean, 12.0);
  ASSERT_TRUE(e.stddev.has_value());
  EXPECT_NEAR(*e.stddev, std::sqrt(8.0), 1e-12);
  ASSERT_TRUE(e.half_width.has_value());
  EXPECT_NEAR(*e.half_width, 12.706 * std::sqrt(8.0) / std::sqrt(2.0), 1e-9);
}

TEST(EstimateOf, AKnownSetMatchesTheTextbook) {
  // 2 4 4 4 5 5 7 9: mean 5, sum of squares 32, sample variance 32/7.
  const std::vector<double> samples = {2, 4, 4, 4, 5, 5, 7, 9};
  const Estimate e = estimate(samples);
  EXPECT_DOUBLE_EQ(e.mean, 5.0);
  EXPECT_NEAR(*e.stddev, std::sqrt(32.0 / 7.0), 1e-12);
  EXPECT_NEAR(*e.half_width, 2.365 * std::sqrt(32.0 / 7.0) / std::sqrt(8.0), 1e-12);
}

TEST(EstimateOf, IsTheSameBitsForTheSameSamples) {
  const std::vector<double> samples = {0.1, 0.2, 0.3, 0.7, 1e-9, 12345.678};
  EXPECT_TRUE(estimate(samples) == estimate(samples));
}

TEST(EstimateEquality, EveryMemberCounts) {
  const Estimate base{3, 2.0, 0.5, 0.25};
  EXPECT_TRUE(base == Estimate(base));
  Estimate other = base;
  other.replicates = 4;
  EXPECT_FALSE(base == other);
  other = base;
  other.mean = 2.5;
  EXPECT_FALSE(base == other);
  other = base;
  other.stddev = 0.6;
  EXPECT_FALSE(base == other);
  other = base;
  other.half_width = std::nullopt;
  EXPECT_FALSE(base == other);
}

TEST(WorstCaseOf, NeedsTwoReplicates) {
  EXPECT_FALSE(worst_case_of("a", {}).has_value());
  const std::vector<double> one = {80.0};
  EXPECT_FALSE(worst_case_of("a", one).has_value());
}

TEST(WorstCaseOf, CarriesTheCountTheSpreadAndTheInterval) {
  const std::vector<double> samples = {80.0, 82.0, 84.0};
  const auto claim = worst_case_of("word_sum", samples);
  ASSERT_TRUE(claim.has_value());
  EXPECT_EQ(claim->candidate, "word_sum");
  EXPECT_EQ(claim->replicates, 3U);
  EXPECT_DOUBLE_EQ(claim->mean, 82.0);
  EXPECT_DOUBLE_EQ(claim->stddev, 2.0);
  EXPECT_NEAR(claim->half_width, 4.303 * 2.0 / std::sqrt(3.0), 1e-12);
}

TEST(EstimateOf, AReferenceThatAddsAlmostNothingLeavesTheReplicatesDegrees) {
  // Eight replicates; a reference that adds 1e-6 to the variance on one degree of
  // freedom leaves the Welch-Satterthwaite degrees at 7 (t 2.365), where the smaller of
  // the two would have taken t at one degree, 12.706.
  const std::vector<double> samples = {80.0, 82.0, 84.0, 82.0, 80.0, 82.0, 84.0, 82.0};
  const Estimate plain = estimate(samples);
  Correction correction;
  correction.variance = 1e-6;
  correction.max_degrees = 1;
  const Estimate corrected = estimate(samples, correction);
  ASSERT_TRUE(plain.half_width.has_value());
  ASSERT_TRUE(corrected.half_width.has_value());
  const double se = *plain.stddev / std::sqrt(8.0);
  EXPECT_NEAR(*plain.half_width, 2.365 * se, 1e-9);
  EXPECT_NEAR(*corrected.half_width, 2.365 * std::sqrt((se * se) + 1e-6), 1e-9);
}

TEST(EstimateOf, AReferenceThatIsAllTheVarianceGivesItsOwnDegrees) {
  // Identical replicates: all the uncertainty is the reference's, so its degrees set t.
  const std::vector<double> samples = {50.0, 50.0, 50.0, 50.0, 50.0};
  Correction correction;
  correction.variance = 4.0;
  correction.max_degrees = 2;
  const Estimate e = estimate(samples, correction);
  ASSERT_TRUE(e.half_width.has_value());
  EXPECT_NEAR(*e.half_width, 4.303 * 2.0, 1e-12);
}

TEST(EstimateOf, TwoEqualSharesGetDegreesBetweenEachAndTheirSum) {
  // Equal shares of 0.2 on 4 and 2 degrees: (0.4)^2 / (0.04/4 + 0.04/2) = 5.33, floored
  // to 5 (t 2.571).
  const std::vector<double> samples = {9.0, 11.0, 9.0, 11.0, 10.0};  // n 5, variance 1
  Correction correction;
  correction.variance = 0.2;  // the sample term is 1 / 5
  correction.max_degrees = 2;
  const Estimate e = estimate(samples, correction);
  ASSERT_TRUE(e.half_width.has_value());
  EXPECT_NEAR(*e.half_width, 2.571 * std::sqrt(0.4), 1e-12);
}

TEST(WorstCaseFormat, SaysCountSpreadAndLevel) {
  const WorstCase claim{"word_sum", 5, 81.25, 1.5, 1.25, 0.5};
  EXPECT_EQ(format(claim), "word_sum: 81.25 +/- 1.25 (95% CI, n=5, sd 1.5, correction sd 0.5)");
}

}  // namespace
}  // namespace loadforge::explore
