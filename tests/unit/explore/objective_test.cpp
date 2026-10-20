// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/objective.hpp"

#include <gtest/gtest.h>

#include <string>

#include "unit/explore/model_runner.hpp"

namespace loadforge::explore {
namespace {

using explore_test::energy_metric;
using explore_test::gone;
using explore_test::model_capabilities;
using explore_test::thermal_metric;
using statistics::CounterSummary;
using statistics::GaugeSummary;
using statistics::LoadSignature;
using statistics::MetricSummary;

Objective objective_for(std::string metric, std::variant<GaugeStatistic, CounterStatistic> stat,
                        std::uint32_t index = 0) {
  Objective o;
  o.metric = std::move(metric);
  o.statistic = stat;
  o.scope_index = index;
  return o;
}

LoadSignature signature_with(MetricSummary summary) {
  LoadSignature sig;
  sig.window = {0, 1000};
  sig.metrics.push_back(std::move(summary));
  return sig;
}

const GaugeSummary kGauge{10, 40.0, 60.0, 50.0, 3.0, 45.0, 58.0, 0.25};

TEST(Describe, EveryWordIsDistinctAndTheUnrecognisedAreNamed) {
  EXPECT_EQ(describe(GaugeStatistic::kMean), "mean");
  EXPECT_EQ(describe(GaugeStatistic::kMax), "max");
  EXPECT_EQ(describe(GaugeStatistic::kMin), "min");
  EXPECT_EQ(describe(GaugeStatistic::kLast), "last");
  EXPECT_EQ(describe(GaugeStatistic::kStddev), "stddev");
  EXPECT_EQ(describe(GaugeStatistic::kSlope), "slope per second");
  EXPECT_EQ(describe(static_cast<GaugeStatistic>(200)), "unrecognised gauge statistic");
  EXPECT_EQ(describe(CounterStatistic::kRate), "rate per second");
  EXPECT_EQ(describe(CounterStatistic::kDelta), "delta");
  EXPECT_EQ(describe(static_cast<CounterStatistic>(200)), "unrecognised counter statistic");
  EXPECT_EQ(describe(Direction::kHigherIsWorse), "higher is worse");
  EXPECT_EQ(describe(Direction::kLowerIsWorse), "lower is worse");
  EXPECT_EQ(describe(static_cast<Direction>(200)), "unrecognised direction");
  EXPECT_EQ(sign_of(Direction::kHigherIsWorse), 1.0);
  EXPECT_EQ(sign_of(Direction::kLowerIsWorse), -1.0);
}

TEST(Describe, EveryFaultHasItsOwnWord) {
  EXPECT_EQ(describe(ExploreFault::kUnknownMetric), "objective metric unknown");
  EXPECT_EQ(describe(ExploreFault::kMetricUnavailable), "objective metric unavailable");
  EXPECT_EQ(describe(ExploreFault::kStatistic), "objective statistic does not fit the metric");
  EXPECT_EQ(describe(ExploreFault::kNoReading), "no reading for the objective");
  EXPECT_EQ(describe(ExploreFault::kConfiguration), "search not well formed");
  EXPECT_EQ(describe(ExploreFault::kDutyCycle), "candidates differ in verification duty cycle");
  EXPECT_EQ(describe(ExploreFault::kTrial), "trial failed");
  EXPECT_EQ(describe(ExploreFault::kReference), "reference unusable");
  EXPECT_EQ(describe(static_cast<ExploreFault>(200)), "unrecognised explore fault");
}

TEST(Bind, AMetricThatIsPresentBinds) {
  const auto bound =
      bind(objective_for("thermal.package", GaugeStatistic::kMax), model_capabilities());
  ASSERT_TRUE(bound.has_value()) << bound.error().detail;
  EXPECT_EQ(bound.value().metric, thermal_metric(0));
  EXPECT_EQ(std::get<GaugeStatistic>(bound.value().statistic), GaugeStatistic::kMax);
  EXPECT_EQ(bound.value().direction, Direction::kHigherIsWorse);
}

TEST(Bind, TheScopeIndexSelectsTheSeries) {
  const auto bound =
      bind(objective_for("thermal.package", GaugeStatistic::kMean, 1), model_capabilities());
  ASSERT_TRUE(bound.has_value());
  EXPECT_EQ(bound.value().metric.scope_index, 1U);
}

TEST(Bind, ACounterMetricBindsToACounterStatistic) {
  const auto bound =
      bind(objective_for("power.energy", CounterStatistic::kRate), model_capabilities());
  ASSERT_TRUE(bound.has_value());
  EXPECT_EQ(bound.value().metric, energy_metric());
}

TEST(Bind, AMetricNotInTheReportFailsAtConfiguration) {
  const auto bound = bind(objective_for("fan.cpu", GaugeStatistic::kMean), model_capabilities());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().fault, ExploreFault::kUnknownMetric);
  EXPECT_NE(bound.error().detail.find("fan.cpu"), std::string::npos);
  EXPECT_FALSE(bound.error().trial.has_value());
}

TEST(Bind, AScopeIndexTheMachineLacksIsUnknownToo) {
  const auto bound =
      bind(objective_for("thermal.package", GaugeStatistic::kMean, 7), model_capabilities());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().fault, ExploreFault::kUnknownMetric);
  EXPECT_NE(bound.error().detail.find("index 7"), std::string::npos);
}

TEST(Bind, AMetricThisMachineCannotReadCarriesTheSourcesRemedy) {
  const auto bound =
      bind(objective_for("thermal.vrm", GaugeStatistic::kMean), model_capabilities());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().fault, ExploreFault::kMetricUnavailable);
  EXPECT_NE(bound.error().detail.find("no such sensor"), std::string::npos);
  EXPECT_NE(bound.error().detail.find("load the coretemp module"), std::string::npos);
}

TEST(Bind, AGaugeStatisticOnACounterIsRefused) {
  const auto bound =
      bind(objective_for("power.energy", GaugeStatistic::kMean), model_capabilities());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().fault, ExploreFault::kStatistic);
  EXPECT_NE(bound.error().detail.find("mean"), std::string::npos);
}

TEST(Bind, ACounterStatisticOnAGaugeIsRefused) {
  const auto bound =
      bind(objective_for("thermal.package", CounterStatistic::kDelta), model_capabilities());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().fault, ExploreFault::kStatistic);
  EXPECT_NE(bound.error().detail.find("delta"), std::string::npos);
}

BoundObjective bound_gauge(GaugeStatistic stat) {
  return BoundObjective{thermal_metric(0), stat, Direction::kHigherIsWorse};
}

TEST(Score, EachGaugeStatisticReadsItsField) {
  const LoadSignature sig = signature_with({thermal_metric(0), kGauge, 0});
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kMean), sig).value(), 50.0);
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kMax), sig).value(), 60.0);
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kMin), sig).value(), 40.0);
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kLast), sig).value(), 58.0);
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kStddev), sig).value(), 3.0);
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kSlope), sig).value(), 0.25);
}

TEST(Score, ABadStatisticValueIsNotAScore) {
  const LoadSignature sig = signature_with({thermal_metric(0), kGauge, 0});
  const auto none = score(bound_gauge(static_cast<GaugeStatistic>(200)), sig);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, ExploreFault::kNoReading);
}

TEST(Score, TheOtherSeriesOfTheSameNameIsNotTheObjectives) {
  const LoadSignature sig = signature_with({thermal_metric(1), kGauge, 0});
  const auto result = score(bound_gauge(GaugeStatistic::kMean), sig);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
  EXPECT_NE(result.error().detail.find("no series"), std::string::npos);
}

TEST(Score, ABadCounterStatisticIsNotAScoreEither) {
  const LoadSignature sig = signature_with({energy_metric(), CounterSummary{4, 10, 2.0}, 0});
  const BoundObjective bad{energy_metric(), static_cast<CounterStatistic>(200),
                           Direction::kHigherIsWorse};
  const auto none = score(bad, sig);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, ExploreFault::kNoReading);
}

TEST(Score, ASeriesAfterAnotherIsFound) {
  LoadSignature sig = signature_with({energy_metric(), CounterSummary{4, 10, 2.0}, 0});
  sig.metrics.push_back({thermal_metric(0), kGauge, 0});
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kMean), sig).value(), 50.0);
}

TEST(Score, ASpreadOfOneReadingIsAnErrorNotZero) {
  const GaugeSummary single{1, 50.0, 50.0, 50.0, std::nullopt, 50.0, 50.0, std::nullopt};
  const LoadSignature sig = signature_with({thermal_metric(0), single, 0});
  EXPECT_EQ(score(bound_gauge(GaugeStatistic::kMean), sig).value(), 50.0);
  for (const GaugeStatistic stat : {GaugeStatistic::kStddev, GaugeStatistic::kSlope}) {
    const auto result = score(bound_gauge(stat), sig);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
    EXPECT_NE(result.error().detail.find("too few readings"), std::string::npos);
  }
}

TEST(Score, ASensorThatDroppedOutIsNotAZero) {
  const LoadSignature sig =
      signature_with({thermal_metric(0), gone(topology::Availability::kVanished), 4});
  const auto result = score(bound_gauge(GaugeStatistic::kMean), sig);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
  EXPECT_NE(result.error().detail.find("no such sensor"), std::string::npos);
  EXPECT_NE(result.error().detail.find("load the coretemp module"), std::string::npos);
}

TEST(Score, ACounterReadsItsRateAndDelta) {
  const LoadSignature sig = signature_with({energy_metric(), CounterSummary{4, 1000, 12.5}, 0});
  const BoundObjective rate{energy_metric(), CounterStatistic::kRate, Direction::kHigherIsWorse};
  const BoundObjective delta{energy_metric(), CounterStatistic::kDelta, Direction::kLowerIsWorse};
  EXPECT_EQ(score(rate, sig).value(), 12.5);
  EXPECT_EQ(score(delta, sig).value(), 1000.0);
}

TEST(Score, ACounterWithNoPairHasNoRate) {
  const LoadSignature sig =
      signature_with({energy_metric(), CounterSummary{1, 0, std::nullopt}, 0});
  const BoundObjective rate{energy_metric(), CounterStatistic::kRate, Direction::kHigherIsWorse};
  const auto result = score(rate, sig);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
}

TEST(Score, ASummaryOfTheWrongKindIsRefused) {
  const BoundObjective counter_objective{energy_metric(), CounterStatistic::kRate,
                                         Direction::kHigherIsWorse};
  const auto gauge_given = score(counter_objective, signature_with({energy_metric(), kGauge, 0}));
  ASSERT_FALSE(gauge_given.has_value());
  EXPECT_EQ(gauge_given.error().fault, ExploreFault::kStatistic);
  const auto counter_given =
      score(bound_gauge(GaugeStatistic::kMean),
            signature_with({thermal_metric(0), CounterSummary{2, 5, 1.0}, 0}));
  ASSERT_FALSE(counter_given.has_value());
  EXPECT_EQ(counter_given.error().fault, ExploreFault::kStatistic);
}

}  // namespace
}  // namespace loadforge::explore
