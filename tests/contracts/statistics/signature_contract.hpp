// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_STATISTICS_SIGNATURE_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_STATISTICS_SIGNATURE_CONTRACT_HPP

// Provider-side suite for src/statistics/signature.hpp (ADR 0003, 0004).
//
// SummarizerContract<Subject>: Subject::make() returns
// std::unique_ptr<Summarizer>.
//
// The suite brings its own TimelineView over plain vectors. Its values are
// chosen so every expected statistic is exact in binary floating point: the
// contract fixes the order of every sum, so a provider computes these bits.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "statistics/signature.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::contracts {

namespace signature_suite {

class Series final : public telemetry::TimelineView {
 public:
  void add(telemetry::Metric metric, std::vector<telemetry::Point> points) {
    metrics_.push_back(std::move(metric));
    points_.push_back(std::move(points));
  }
  [[nodiscard]] std::size_t series_count() const noexcept override { return metrics_.size(); }
  [[nodiscard]] const telemetry::Metric& metric(telemetry::SeriesId series) const override {
    return metrics_.at(series);
  }
  [[nodiscard]] std::span<const telemetry::Point> points(
      telemetry::SeriesId series) const override {
    return points_.at(series);
  }
  [[nodiscard]] std::span<const telemetry::PhaseMark> phases() const override { return {}; }

 private:
  std::vector<telemetry::Metric> metrics_;
  std::vector<std::vector<telemetry::Point>> points_;
};

inline constexpr std::int64_t kSecond = 1'000'000'000;

inline telemetry::Metric temperature() {
  return {"thermal.package",
          telemetry::Unit::kCelsius,
          telemetry::Kind::kGauge,
          telemetry::Scope::kPackage,
          0,
          std::nullopt,
          "test"};
}

inline telemetry::Metric energy() {
  return {"power.energy",
          telemetry::Unit::kJoules,
          telemetry::Kind::kCounter,
          telemetry::Scope::kPackage,
          0,
          std::uint64_t{1000},
          "test"};
}

inline telemetry::Unavailable gone(const std::string& detail) {
  return {topology::Availability::kVanished, "/sys/x", detail, "re-load the driver"};
}

}  // namespace signature_suite

template <typename Subject>
class SummarizerContract : public ::testing::Test {
 protected:
  static statistics::LoadSignature summarize(const telemetry::TimelineView& view,
                                             statistics::Window window) {
    const auto signature = Subject::make()->summarize(view, window);
    EXPECT_TRUE(signature.has_value());
    return signature.value_or(statistics::LoadSignature{});
  }
};
TYPED_TEST_SUITE_P(SummarizerContract);

using signature_suite::kSecond;
using statistics::CounterSummary;
using statistics::GaugeSummary;
using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Point;
using telemetry::Unavailable;

TYPED_TEST_P(SummarizerContract, AnEmptyOrInvertedWindowIsRefused) {
  const signature_suite::Series view;
  for (const statistics::Window window : {statistics::Window{5, 5}, statistics::Window{6, 5}}) {
    const auto signature = TypeParam::make()->summarize(view, window);
    ASSERT_FALSE(signature.has_value());
    EXPECT_EQ(signature.error().fault, statistics::StatisticsFault::kWindow);
  }
}

TYPED_TEST_P(SummarizerContract, AGaugeIsSummarisedExactlyOverItsPresentReadings) {
  signature_suite::Series view;
  view.add(signature_suite::temperature(),
           {Point{0, Gauge{1.0}}, Point{kSecond / 2, signature_suite::gone("dropped")},
            Point{kSecond, Gauge{3.0}}, Point{2 * kSecond, Gauge{5.0}}});
  const auto signature = this->summarize(view, {0, 3 * kSecond});
  ASSERT_EQ(signature.metrics.size(), 1U);
  EXPECT_EQ(signature.window, (statistics::Window{0, 3 * kSecond}));
  EXPECT_EQ(signature.metrics[0].metric, signature_suite::temperature());
  EXPECT_EQ(signature.metrics[0].unavailable_samples, 1U);
  const auto* gauge = std::get_if<GaugeSummary>(&signature.metrics[0].value);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(*gauge, (GaugeSummary{3, 1.0, 5.0, 3.0, 2.0, 1.0, 5.0, 2.0}))
      << "mean 3, sample deviation 2, slope 2 per second -- all exact";
}

TYPED_TEST_P(SummarizerContract, TheWindowIncludesItsStartAndExcludesItsEnd) {
  signature_suite::Series view;
  view.add(signature_suite::temperature(),
           {Point{kSecond, Gauge{10.0}}, Point{2 * kSecond, Gauge{20.0}},
            Point{3 * kSecond, Gauge{30.0}}});
  const auto signature = this->summarize(view, {kSecond, 3 * kSecond});
  const auto* gauge = std::get_if<GaugeSummary>(&signature.metrics.at(0).value);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(gauge->samples, 2U);
  EXPECT_EQ(gauge->first, 10.0);
  EXPECT_EQ(gauge->last, 20.0);
}

TYPED_TEST_P(SummarizerContract, OneReadingHasNoDeviationAndNoSlope) {
  signature_suite::Series view;
  view.add(signature_suite::temperature(), {Point{kSecond, Gauge{7.0}}});
  const auto signature = this->summarize(view, {0, 2 * kSecond});
  const auto* gauge = std::get_if<GaugeSummary>(&signature.metrics.at(0).value);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(*gauge, (GaugeSummary{1, 7.0, 7.0, 7.0, std::nullopt, 7.0, 7.0, std::nullopt}));
}

TYPED_TEST_P(SummarizerContract, AMetricWithNoReadingIsUnavailableNeverZero) {
  signature_suite::Series view;
  view.add(signature_suite::temperature(),
           {Point{1, signature_suite::gone("first")}, Point{2, signature_suite::gone("second")},
            Point{10 * kSecond, Gauge{50.0}}});
  view.add(signature_suite::energy(), {});
  const auto signature = this->summarize(view, {0, kSecond});
  ASSERT_EQ(signature.metrics.size(), 2U) << "one summary per series, in series order";
  const auto* dropped = std::get_if<Unavailable>(&signature.metrics[0].value);
  ASSERT_NE(dropped, nullptr);
  EXPECT_EQ(*dropped, signature_suite::gone("second")) << "the last reason in the window";
  EXPECT_EQ(signature.metrics[0].unavailable_samples, 2U);
  const auto* never = std::get_if<Unavailable>(&signature.metrics[1].value);
  ASSERT_NE(never, nullptr);
  EXPECT_EQ(never->kind, topology::Availability::kAbsent);
  EXPECT_EQ(never->detail, "no sample in the window");
  EXPECT_FALSE(never->remediation.empty());
  EXPECT_EQ(signature.metrics[1].metric, signature_suite::energy());
}

TYPED_TEST_P(SummarizerContract, ACounterAdvancesAcrossWrapsButNotAcrossGaps) {
  signature_suite::Series view;
  view.add(signature_suite::energy(),
           {Point{0, Counter{990}}, Point{kSecond, Counter{5}},
            Point{2 * kSecond, signature_suite::gone("reset?")}, Point{3 * kSecond, Counter{700}},
            Point{5 * kSecond, Counter{710}}});
  const auto signature = this->summarize(view, {0, 6 * kSecond});
  const auto* counter = std::get_if<CounterSummary>(&signature.metrics.at(0).value);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(*counter, (CounterSummary{4, 25, 25.0 / 3.0}))
      << "15 across the wrap, 10 after the gap, over the 3 s those two pairs span";
  EXPECT_EQ(signature.metrics[0].unavailable_samples, 1U);
}

TYPED_TEST_P(SummarizerContract, ACounterReadOnceHasNoRate) {
  signature_suite::Series view;
  view.add(signature_suite::energy(), {Point{kSecond, Counter{42}}});
  const auto signature = this->summarize(view, {0, 2 * kSecond});
  const auto* counter = std::get_if<CounterSummary>(&signature.metrics.at(0).value);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(*counter, (CounterSummary{1, 0, std::nullopt}));
}

REGISTER_TYPED_TEST_SUITE_P(SummarizerContract, AnEmptyOrInvertedWindowIsRefused,
                            AGaugeIsSummarisedExactlyOverItsPresentReadings,
                            TheWindowIncludesItsStartAndExcludesItsEnd,
                            OneReadingHasNoDeviationAndNoSlope,
                            AMetricWithNoReadingIsUnavailableNeverZero,
                            ACounterAdvancesAcrossWrapsButNotAcrossGaps, ACounterReadOnceHasNoRate);

}  // namespace loadforge::contracts

#endif
