// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/drift.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

#include "explore/estimate.hpp"

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

// The points must be in time order (a search's reference readings are: each trial is
// later than the one before). Nothing reorders or skips a reading that is not, which
// the test once claimed ("passed over"): with {300, 100} the scan holds the first
// value up to 300 and the last element after it. Review verdicts 2026-10-23, finding
// 12d.
TEST(BaselineAt, AReadingOutOfOrderIsNotReorderedOrSkippedItIsReadAsItLies) {
  const std::vector<ReferencePoint> points = {{300, 5.0}, {100, 9.0}};
  EXPECT_EQ(baseline_at(points, 200), 5.0) << "held at the first element up to its time";
  EXPECT_EQ(baseline_at(points, 400), 9.0) << "the last element, not the latest reading";
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

// The degrees of freedom credited to the reference's noise estimate. The code had
// used readings - 1; the effective degrees of freedom of reference_variance() are
// 1.0, 1.4, 1.9, 2.3 and 3.4 for 3, 4, 5, 6 and 8 readings (review verdicts
// 2026-10-23, finding 8), so a t interval on n - 1 was too narrow.
TEST(ReferenceDegrees, AreWhatTheDerivationSaysAndNeverBelowOne) {
  const std::size_t expected[] = {1, 1, 1, 1, 1, 2, 2, 3, 3, 4, 49};
  const std::size_t readings[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 100};
  for (std::size_t i = 0; i < std::size(readings); ++i) {
    EXPECT_EQ(reference_degrees(readings[i]), expected[i]) << readings[i] << " readings";
  }
}

// Satterthwaite: the degrees of freedom of the chi-square that has the estimator's mean
// and variance, measured over standard normal readings spaced equally in time.
TEST(ReferenceDegrees, DoNotExceedTheEffectiveDegreesOfFreedomOfTheEstimate) {
  std::mt19937_64 rng(88);
  std::normal_distribution<double> noise(0.0, 1.0);
  constexpr int kDraws = 100'000;
  for (std::size_t n = 3; n <= 8; ++n) {
    double sum = 0.0;
    double squares = 0.0;
    for (int draw = 0; draw < kDraws; ++draw) {
      std::vector<ReferencePoint> points;
      for (std::size_t i = 0; i < n; ++i) {
        points.push_back({static_cast<std::int64_t>(i) * 1000, noise(rng)});
      }
      const double variance = reference_variance(points);
      sum += variance;
      squares += variance * variance;
    }
    const double mean = sum / kDraws;
    const double effective = 2.0 * mean * mean / (squares / kDraws - mean * mean);
    EXPECT_NEAR(mean, 1.0, 0.02) << "the estimate is unbiased, n=" << n;
    EXPECT_LE(static_cast<double>(reference_degrees(n)), effective + 0.1)
        << "n=" << n << " effective df " << effective;
  }
}

// Candidate scores that are constant leave the reference's noise as the only error, so
// the interval of the corrected mean must cover the true value (0) at its stated
// level, which it did not with readings - 1 degrees (86% at three readings).
TEST(ReferenceDegrees, AnIntervalOnTheReferencesNoiseCoversAtItsStatedLevel) {
  std::mt19937_64 rng(808);
  std::normal_distribution<double> noise(0.0, 1.0);
  constexpr int kDraws = 40'000;
  constexpr std::size_t kAnchors = 2;
  for (const std::size_t readings : {3U, 5U}) {
    int covered = 0;
    for (int draw = 0; draw < kDraws; ++draw) {
      std::vector<ReferencePoint> points;
      for (std::size_t i = 0; i < readings; ++i) {
        points.push_back({static_cast<std::int64_t>(i) * 1000, noise(rng)});
      }
      const double anchor = (points[0].value + points[1].value) / kAnchors;
      std::vector<std::int64_t> times;
      std::vector<double> samples;
      for (int k = 0; k < 10; ++k) {
        const std::int64_t at = 1500 + (static_cast<std::int64_t>(readings) - 2) * 50 * k;
        times.push_back(at);
        samples.push_back(corrected(0.0, baseline_at(points, at), anchor));
      }
      Correction correction;
      correction.variance = correction_gain(points, times, kAnchors) * reference_variance(points);
      correction.max_degrees = reference_degrees(points.size());
      const Estimate estimate_of_samples = estimate(samples, correction);
      covered += std::abs(estimate_of_samples.mean) <= *estimate_of_samples.half_width ? 1 : 0;
    }
    EXPECT_GE(static_cast<double>(covered) / kDraws, 0.94) << readings << " readings";
  }
}

}  // namespace
}  // namespace loadforge::explore
