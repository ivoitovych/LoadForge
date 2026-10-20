// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1: the Summarizer against the signature contract's suite, and the decisions
// the contract leaves to the implementation.

#include "statistics/summarizer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <variant>

#include "contracts/statistics/signature_contract.hpp"

namespace loadforge::statistics {
namespace {

using contracts::signature_suite::energy;
using contracts::signature_suite::gone;
using contracts::signature_suite::kSecond;
using contracts::signature_suite::Series;
using contracts::signature_suite::temperature;
using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Point;

struct Compensated {
  static std::unique_ptr<Summarizer> make() { return std::make_unique<CompensatedSummarizer>(); }
};

}  // namespace
}  // namespace loadforge::statistics

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Compensated, SummarizerContract,
                               ::testing::Types<statistics::Compensated>);
}  // namespace loadforge::contracts

namespace loadforge::statistics {
namespace {

LoadSignature must_summarize(const Series& view, Window window) {
  const auto signature = CompensatedSummarizer().summarize(view, window);
  EXPECT_TRUE(signature.has_value());
  return signature.value();
}

const GaugeSummary& gauge_of(const LoadSignature& signature, std::size_t index = 0) {
  return std::get<GaugeSummary>(signature.metrics.at(index).value);
}

const CounterSummary& counter_of(const LoadSignature& signature, std::size_t index = 0) {
  return std::get<CounterSummary>(signature.metrics.at(index).value);
}

TEST(Summarizer, AnInvertedWindowSaysWhy) {
  const auto signature = CompensatedSummarizer().summarize(Series{}, {5, 5});
  ASSERT_FALSE(signature.has_value());
  EXPECT_EQ(signature.error().fault, StatisticsFault::kWindow);
  EXPECT_EQ(signature.error().detail, "the window ends no later than it starts");
}

TEST(Summarizer, AViewWithoutSeriesHasAnEmptySignature) {
  const auto signature = must_summarize(Series{}, {0, kSecond});
  EXPECT_TRUE(signature.metrics.empty());
  EXPECT_EQ(signature.window, (Window{0, kSecond}));
}

TEST(Summarizer, PointsBeforeAndAfterTheWindowAreNotRead) {
  Series view;
  view.add(temperature(), {Point{1, Gauge{100.0}}, Point{kSecond, Gauge{2.0}},
                           Point{2 * kSecond, Gauge{4.0}}, Point{9 * kSecond, Gauge{100.0}}});
  const auto signature = must_summarize(view, {kSecond, 3 * kSecond});
  EXPECT_EQ(gauge_of(signature), (GaugeSummary{2, 2.0, 4.0, 3.0, std::sqrt(2.0), 2.0, 4.0, 2.0}));
}

TEST(Summarizer, AWindowBeforeEveryPointAndOneAfterEveryPointFindNothing) {
  Series view;
  view.add(temperature(), {Point{5 * kSecond, Gauge{1.0}}});
  for (const Window window : {Window{0, kSecond}, Window{6 * kSecond, 7 * kSecond}}) {
    const auto signature = must_summarize(view, window);
    const auto* absent = std::get_if<telemetry::Unavailable>(&signature.metrics.at(0).value);
    ASSERT_NE(absent, nullptr);
    EXPECT_EQ(absent->kind, topology::Availability::kAbsent);
    EXPECT_EQ(signature.metrics[0].unavailable_samples, 0U);
  }
}

TEST(Summarizer, ASumIsCompensatedSoAHugeValueDoesNotSwallowASmallOne) {
  // Plain summation of 1e16, 1, 1, -1e16 gives 0; the compensated sum gives 2.
  Series view;
  view.add(temperature(), {Point{1, Gauge{1e16}}, Point{2, Gauge{1.0}}, Point{3, Gauge{1.0}},
                           Point{4, Gauge{-1e16}}});
  const auto signature = must_summarize(view, {0, kSecond});
  EXPECT_EQ(gauge_of(signature).mean, 0.5);
}

TEST(Summarizer, TheSmallerOperandIsCompensatedToo) {
  // The other arm of the compensation: the running sum is smaller than the value.
  Series view;
  view.add(temperature(), {Point{1, Gauge{1.0}}, Point{2, Gauge{1e16}}, Point{3, Gauge{1.0}},
                           Point{4, Gauge{-1e16}}});
  const auto signature = must_summarize(view, {0, kSecond});
  EXPECT_EQ(gauge_of(signature).mean, 0.5);
}

TEST(Summarizer, ReadingsAtOneInstantHaveNoSlope) {
  // A view that breaks the timeline's ordering rule: no variance of time, no slope.
  Series view;
  view.add(temperature(), {Point{5, Gauge{1.0}}, Point{5, Gauge{3.0}}});
  const auto signature = must_summarize(view, {0, kSecond});
  EXPECT_EQ(gauge_of(signature).stddev, std::sqrt(2.0));
  EXPECT_EQ(gauge_of(signature).slope_per_second, std::nullopt);
}

TEST(Summarizer, AReadingOfTheWrongKindForItsMetricIsNotCounted) {
  Series view;
  view.add(temperature(), {Point{1, Counter{9}}, Point{2, Gauge{4.0}}});
  view.add(energy(), {Point{1, Gauge{9.0}}, Point{2, Counter{4}}});
  const auto signature = must_summarize(view, {0, kSecond});
  EXPECT_EQ(gauge_of(signature, 0).samples, 1U);
  EXPECT_EQ(gauge_of(signature, 0).first, 4.0);
  EXPECT_EQ(counter_of(signature, 1), (CounterSummary{1, 0, std::nullopt}));
}

TEST(Summarizer, AMisfiledReadingAloneLeavesTheSeriesAbsent) {
  Series view;
  view.add(temperature(), {Point{1, Counter{9}}});
  const auto signature = must_summarize(view, {0, kSecond});
  const auto* absent = std::get_if<telemetry::Unavailable>(&signature.metrics.at(0).value);
  ASSERT_NE(absent, nullptr);
  EXPECT_EQ(absent->detail, "no sample in the window");
}

TEST(Summarizer, ACounterWithoutAModulusWrapsAtTwoToTheSixtyFour) {
  telemetry::Metric metric = energy();
  metric.modulus = std::nullopt;
  Series view;
  view.add(metric, {Point{0, Counter{std::numeric_limits<std::uint64_t>::max() - 1}},
                    Point{kSecond, Counter{3}}});
  const auto signature = must_summarize(view, {0, 2 * kSecond});
  EXPECT_EQ(counter_of(signature), (CounterSummary{2, 5, 5.0}));
}

TEST(Summarizer, ACounterReadingJustAfterAGapStartsAFreshRun) {
  Series view;
  view.add(energy(),
           {Point{0, Counter{10}}, Point{kSecond, gone("x")}, Point{2 * kSecond, Counter{500}},
            Point{3 * kSecond, gone("y")}, Point{4 * kSecond, Counter{1}}});
  const auto signature = must_summarize(view, {0, 5 * kSecond});
  EXPECT_EQ(counter_of(signature), (CounterSummary{3, 0, std::nullopt}))
      << "no two present readings are consecutive, so nothing advanced that is known";
  EXPECT_EQ(signature.metrics[0].unavailable_samples, 2U);
}

TEST(Summarizer, AGaugeWithOnlyGapsInTheWindowReportsTheLastReason) {
  Series view;
  view.add(temperature(), {Point{1, gone("a")}, Point{2, gone("b")}});
  const auto signature = must_summarize(view, {0, kSecond});
  EXPECT_EQ(std::get<telemetry::Unavailable>(signature.metrics.at(0).value), gone("b"));
}

}  // namespace
}  // namespace loadforge::statistics
