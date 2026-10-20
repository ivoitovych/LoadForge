// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: metamorphic relations of the Summarizer. Each builds a series from a
// fixed pseudo-random sequence, transforms it or its window in a way whose
// effect on the signature is known, and checks the known effect.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "contracts/statistics/signature_contract.hpp"
#include "statistics/summarizer.hpp"

// The contract header registers SummarizerContract; this binary relates signatures
// to each other and runs the suite itself in tests/unit/statistics.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(SummarizerContract);

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

/// A fixed generator, so every run of the suite sees the same series.
class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_ >> 33;
  }

 private:
  std::uint64_t state_;
};

constexpr int kTrials = 50;
constexpr int kPoints = 40;

std::vector<Point> random_gauges(Lcg& rng, std::int64_t offset_ns, double bias) {
  std::vector<Point> points;
  std::int64_t time = offset_ns;
  for (int i = 0; i < kPoints; ++i) {
    time += static_cast<std::int64_t>(1 + rng.next() % 5) * (kSecond / 4);
    points.push_back(Point{time, Gauge{bias + static_cast<double>(rng.next() % 4000) / 8.0}});
  }
  return points;
}

std::vector<Point> random_counters(Lcg& rng, std::uint64_t base, bool with_gaps) {
  std::vector<Point> points;
  std::int64_t time = 0;
  std::uint64_t raw = base;
  for (int i = 0; i < kPoints; ++i) {
    time += static_cast<std::int64_t>(1 + rng.next() % 5) * (kSecond / 4);
    raw = (raw + rng.next() % 50) % 1000;
    const bool gap = with_gaps && rng.next() % 6 == 0;
    points.push_back(gap ? Point{time, gone("g")} : Point{time, Counter{raw}});
  }
  return points;
}

LoadSignature summarize_one(const telemetry::Metric& metric, std::vector<Point> points,
                            Window window) {
  Series view;
  view.add(metric, std::move(points));
  const auto signature = CompensatedSummarizer().summarize(view, window);
  EXPECT_TRUE(signature.has_value());
  return signature.value();
}

constexpr Window kAll{0, 1'000'000 * kSecond};

TEST(StatisticsMetamorphic, ACounterWindowSplitInTwoSumsBackWithTheStepAcrossTheSeam) {
  Lcg rng(1);
  for (int trial = 0; trial < kTrials; ++trial) {
    // No gaps: the step across the seam is then a known consecutive pair.
    const auto points = random_counters(rng, rng.next() % 1000, false);
    const std::int64_t seam = points[10 + trial % 20].timestamp_ns;
    const auto whole =
        std::get<CounterSummary>(summarize_one(energy(), points, kAll).metrics.at(0).value);
    const auto left =
        std::get<CounterSummary>(summarize_one(energy(), points, {0, seam}).metrics.at(0).value);
    const auto right = std::get<CounterSummary>(
        summarize_one(energy(), points, {seam, kAll.end_ns}).metrics.at(0).value);
    const auto seam_index = static_cast<std::size_t>(
        std::find_if(points.begin(), points.end(),
                     [seam](const Point& p) { return p.timestamp_ns == seam; }) -
        points.begin());
    const auto before = std::get<Counter>(points[seam_index - 1].reading).raw;
    const auto after = std::get<Counter>(points[seam_index].reading).raw;
    EXPECT_EQ(left.samples + right.samples, whole.samples);
    EXPECT_EQ(left.delta + right.delta + telemetry::counter_delta(before, after, 1000),
              whole.delta);
  }
}

TEST(StatisticsMetamorphic, AGaugeWindowSplitInTwoKeepsCountExtremesAndMean) {
  Lcg rng(2);
  for (int trial = 0; trial < kTrials; ++trial) {
    const auto points = random_gauges(rng, 0, 0.0);
    const std::int64_t seam = points[5 + trial % 30].timestamp_ns;
    const auto whole =
        std::get<GaugeSummary>(summarize_one(temperature(), points, kAll).metrics.at(0).value);
    const auto left =
        std::get<GaugeSummary>(summarize_one(temperature(), points, {0, seam}).metrics.at(0).value);
    const auto right = std::get<GaugeSummary>(
        summarize_one(temperature(), points, {seam, kAll.end_ns}).metrics.at(0).value);
    EXPECT_EQ(left.samples + right.samples, whole.samples);
    EXPECT_EQ(std::min(left.min, right.min), whole.min);
    EXPECT_EQ(std::max(left.max, right.max), whole.max);
    EXPECT_EQ(left.first, whole.first);
    EXPECT_EQ(right.last, whole.last);
    const double recombined = (left.mean * static_cast<double>(left.samples) +
                               right.mean * static_cast<double>(right.samples)) /
                              static_cast<double>(whole.samples);
    EXPECT_NEAR(recombined, whole.mean, 1e-9);
  }
}

TEST(StatisticsMetamorphic, AConstantSeriesHasNoSpreadAndNoSlope) {
  Lcg rng(3);
  for (int trial = 0; trial < kTrials; ++trial) {
    auto points = random_gauges(rng, 0, 0.0);
    const double constant = static_cast<double>(rng.next() % 1000) / 4.0;
    for (Point& point : points) {
      point.reading = Gauge{constant};
    }
    const auto gauge =
        std::get<GaugeSummary>(summarize_one(temperature(), points, kAll).metrics.at(0).value);
    EXPECT_EQ(gauge.min, constant);
    EXPECT_EQ(gauge.max, constant);
    EXPECT_NEAR(gauge.mean, constant, 1e-12);
    EXPECT_NEAR(*gauge.stddev, 0.0, 1e-12);
    EXPECT_NEAR(*gauge.slope_per_second, 0.0, 1e-12);
  }
}

TEST(StatisticsMetamorphic, ASeriesShiftedInTimeWithItsWindowHasTheSameSignature) {
  Lcg rng(4);
  for (int trial = 0; trial < kTrials; ++trial) {
    const std::int64_t shift = static_cast<std::int64_t>(rng.next() % 1000) * kSecond;
    const auto gauges = random_gauges(rng, 0, 0.0);
    const auto counters = random_counters(rng, 3, true);
    const Window window{kSecond, 30 * kSecond};
    auto shifted_gauges = gauges;
    auto shifted_counters = counters;
    for (Point& point : shifted_gauges) {
      point.timestamp_ns += shift;
    }
    for (Point& point : shifted_counters) {
      point.timestamp_ns += shift;
    }
    const Window shifted_window{window.start_ns + shift, window.end_ns + shift};
    const auto a = summarize_one(temperature(), gauges, window);
    const auto b = summarize_one(temperature(), shifted_gauges, shifted_window);
    EXPECT_EQ(a.metrics.at(0).value, b.metrics.at(0).value) << "bit for bit";
    const auto c = summarize_one(energy(), counters, window);
    const auto d = summarize_one(energy(), shifted_counters, shifted_window);
    EXPECT_EQ(c.metrics.at(0).value, d.metrics.at(0).value) << "bit for bit";
  }
}

TEST(StatisticsMetamorphic, ACounterOffsetByAConstantAdvancesTheSame) {
  Lcg rng(5);
  telemetry::Metric metric = energy();
  metric.modulus = std::nullopt;
  for (int trial = 0; trial < kTrials; ++trial) {
    std::vector<Point> points;
    std::uint64_t raw = 0;
    std::int64_t time = 0;
    for (int i = 0; i < kPoints; ++i) {
      time += kSecond / 2;
      raw += rng.next() % 100;
      points.push_back(Point{time, Counter{raw}});
    }
    auto offset = points;
    const std::uint64_t constant = rng.next() % 1'000'000;
    for (Point& point : offset) {
      point.reading = Counter{std::get<Counter>(point.reading).raw + constant};
    }
    EXPECT_EQ(summarize_one(metric, points, kAll).metrics.at(0).value,
              summarize_one(metric, offset, kAll).metrics.at(0).value);
  }
}

TEST(StatisticsMetamorphic, AGaugeOffsetByAConstantMovesOnlyItsLevel) {
  Lcg rng(6);
  for (int trial = 0; trial < kTrials; ++trial) {
    const auto points = random_gauges(rng, 0, 0.0);
    const double constant = static_cast<double>(rng.next() % 100) * 8.0;
    auto offset = points;
    for (Point& point : offset) {
      point.reading = Gauge{std::get<Gauge>(point.reading).value + constant};
    }
    const auto a =
        std::get<GaugeSummary>(summarize_one(temperature(), points, kAll).metrics.at(0).value);
    const auto b =
        std::get<GaugeSummary>(summarize_one(temperature(), offset, kAll).metrics.at(0).value);
    EXPECT_EQ(b.min, a.min + constant);
    EXPECT_EQ(b.max, a.max + constant);
    EXPECT_NEAR(b.mean, a.mean + constant, 1e-9);
    EXPECT_NEAR(*b.stddev, *a.stddev, 1e-9);
    EXPECT_NEAR(*b.slope_per_second, *a.slope_per_second, 1e-9);
  }
}

}  // namespace
}  // namespace loadforge::statistics
