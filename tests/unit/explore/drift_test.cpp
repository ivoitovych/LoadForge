// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/drift.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace loadforge::explore {
namespace {

TEST(BaselineAt, NoReferenceIsZero) { EXPECT_EQ(baseline_at({}, 100), 0.0); }

TEST(BaselineAt, HoldsTheFirstReadingBeforeIt) {
  const std::vector<ReferencePoint> points = {{100, 10.0}, {200, 20.0}};
  EXPECT_EQ(baseline_at(points, 0), 10.0);
  EXPECT_EQ(baseline_at(points, 100), 10.0);
}

TEST(BaselineAt, InterpolatesLinearlyBetweenNeighbours) {
  const std::vector<ReferencePoint> points = {{100, 10.0}, {200, 20.0}, {400, 10.0}};
  EXPECT_DOUBLE_EQ(baseline_at(points, 150), 15.0);
  EXPECT_DOUBLE_EQ(baseline_at(points, 200), 20.0);
  EXPECT_DOUBLE_EQ(baseline_at(points, 300), 15.0);
  EXPECT_DOUBLE_EQ(baseline_at(points, 400), 10.0);
}

TEST(BaselineAt, HoldsTheLastReadingAfterIt) {
  const std::vector<ReferencePoint> points = {{100, 10.0}, {200, 20.0}};
  EXPECT_EQ(baseline_at(points, 201), 20.0);
  EXPECT_EQ(baseline_at(points, 1'000'000'000'000), 20.0);
}

TEST(BaselineAt, TwoReadingsAtOneTimeNeverDivideByZero) {
  const std::vector<ReferencePoint> points = {{100, 10.0}, {200, 20.0}, {200, 30.0}, {300, 30.0}};
  EXPECT_EQ(baseline_at(points, 150), 15.0);
  EXPECT_EQ(baseline_at(points, 200), 20.0);
  EXPECT_EQ(baseline_at(points, 250), 30.0);
  const std::vector<ReferencePoint> same = {{100, 10.0}, {100, 12.0}, {300, 12.0}};
  EXPECT_EQ(baseline_at(same, 100), 10.0);
  EXPECT_EQ(baseline_at(same, 101), 12.0);
}

TEST(BaselineAt, AReadingOutOfOrderIsPassedOver) {
  const std::vector<ReferencePoint> points = {{300, 5.0}, {100, 9.0}};
  EXPECT_EQ(baseline_at(points, 200), 5.0);
  EXPECT_EQ(baseline_at(points, 400), 9.0);
}

TEST(BracketAt, NamesTheNeighboursAndThePlaceBetweenThem) {
  const std::vector<ReferencePoint> points = {{100, 1.0}, {200, 2.0}, {400, 3.0}};
  const Bracket before = bracket_at(points, 50);
  EXPECT_EQ(before.lower, 0U);
  EXPECT_EQ(before.upper, 0U);
  EXPECT_EQ(before.fraction, 0.0);
  const Bracket inside = bracket_at(points, 300);
  EXPECT_EQ(inside.lower, 1U);
  EXPECT_EQ(inside.upper, 2U);
  EXPECT_DOUBLE_EQ(inside.fraction, 0.5);
  const Bracket after = bracket_at(points, 900);
  EXPECT_EQ(after.lower, 2U);
  EXPECT_EQ(after.upper, 2U);
}

TEST(ReferenceVariance, NeedsTwoReadings) {
  EXPECT_EQ(reference_variance({}), 0.0);
  const std::vector<ReferencePoint> one = {{100, 5.0}};
  EXPECT_EQ(reference_variance(one), 0.0);
}

TEST(ReferenceVariance, TwoReadingsGiveHalfTheSquaredDifference) {
  const std::vector<ReferencePoint> two = {{100, 5.0}, {200, 9.0}};
  EXPECT_DOUBLE_EQ(reference_variance(two), 8.0);
}

TEST(ReferenceVariance, ALinearDriftIsNotNoise) {
  const std::vector<ReferencePoint> ramp = {
      {0, 10.0}, {100, 12.0}, {250, 15.0}, {300, 16.0}, {700, 24.0}};
  EXPECT_NEAR(reference_variance(ramp), 0.0, 1e-12);
}

TEST(ReferenceVariance, ASteadyReferenceWithNoiseMeasuresIt) {
  // Equal spacing: w = 1/2, residual of a middle reading is r1 - (r0 + r2)/2,
  // scaled by 1.5. Readings 10, 12, 10, 8, 10: residuals 2, -2, 2... computed:
  // (12 - 10)=2, (10 - 10)=0, (8 - 10)=-2 -> (4 + 0 + 4) / 1.5 / 3.
  const std::vector<ReferencePoint> noisy = {{0, 10.0}, {1, 12.0}, {2, 10.0}, {3, 8.0}, {4, 10.0}};
  EXPECT_NEAR(reference_variance(noisy), (4.0 + 0.0 + 4.0) / 1.5 / 3.0, 1e-12);
}

TEST(ReferenceVariance, ReadingsAtOneTimeAreNeighboursHalfway) {
  const std::vector<ReferencePoint> same = {{5, 10.0}, {5, 13.0}, {5, 10.0}};
  EXPECT_NEAR(reference_variance(same), 3.0 * 3.0 / 1.5, 1e-12);
}

TEST(CorrectionGain, WithNoTrialsItIsTheAnchorsAlone) {
  const std::vector<ReferencePoint> points = {{0, 1.0}, {10, 1.0}, {20, 1.0}};
  EXPECT_DOUBLE_EQ(correction_gain(points, {}, 1), 1.0);
  EXPECT_DOUBLE_EQ(correction_gain(points, {}, 2), 0.25 + 0.25);
}

TEST(CorrectionGain, ATrialAtTheAnchorCancelsIt) {
  const std::vector<ReferencePoint> points = {{0, 1.0}, {10, 1.0}, {20, 1.0}};
  const std::vector<std::int64_t> at_anchor = {0};
  EXPECT_DOUBLE_EQ(correction_gain(points, at_anchor, 1), 0.0);
}

TEST(CorrectionGain, ATrialBetweenReadingsSharesTheirWeights) {
  const std::vector<ReferencePoint> points = {{0, 1.0}, {10, 1.0}, {20, 1.0}};
  const std::vector<std::int64_t> halfway = {5};
  // Weights: reading 0 gets 1/2 less the anchor's 1, reading 1 gets 1/2.
  EXPECT_DOUBLE_EQ(correction_gain(points, halfway, 1), 0.25 + 0.25);
}

TEST(CorrectionGain, ManyTrialsAverageTheBaselineNoiseDown) {
  std::vector<ReferencePoint> points;
  std::vector<std::int64_t> trials;
  points.reserve(41);
  trials.reserve(400);
  for (std::int64_t k = 0; k < 41; ++k) {
    points.push_back({k * 10, 1.0});
  }
  for (std::int64_t k = 0; k < 400; ++k) {
    trials.push_back(k + 5);
  }
  // 400 trials spread over 40 readings: the baseline's noise shrinks to about
  // 1/40 of a reading's; what is left is mostly the anchor (reading 0 alone).
  const double variance = correction_gain(points, trials, 1);
  EXPECT_LT(variance, 1.1);
  EXPECT_GT(variance, 0.0);
}

TEST(Corrected, TakesOutTheDriftSinceTheAnchor) {
  EXPECT_DOUBLE_EQ(corrected(75.0, 60.0, 50.0), 65.0);
  EXPECT_DOUBLE_EQ(corrected(75.0, 50.0, 50.0), 75.0);
  EXPECT_DOUBLE_EQ(corrected(75.0, 45.0, 50.0), 80.0);
}

}  // namespace
}  // namespace loadforge::explore
