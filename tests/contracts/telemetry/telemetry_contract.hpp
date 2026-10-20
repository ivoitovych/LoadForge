// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_TELEMETRY_TELEMETRY_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_TELEMETRY_TELEMETRY_CONTRACT_HPP

// Provider-side suites for src/telemetry/source.hpp and timeline.hpp
// (ADR 0003, 0004).
//
// SourceContract<Subject>:
//   Subject::make()   std::unique_ptr<Source>, built over whatever fixture the
//                     subject chooses. A provider instantiates the suite once
//                     per fixture state -- present, absent, denied, malformed,
//                     vanished, wrapped -- because each must keep the
//                     contract; the hostile-fixture assertions about WHICH
//                     state was reported are the provider's own tests.
//   Subject::kRounds  how many times to sample (at least 1).
//
// TimelineContract<Subject>:
//   Subject::make(capacity)  std::unique_ptr<Timeline> holding at most
//                            `capacity` points.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::contracts {

template <typename Subject>
class SourceContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(SourceContract);

using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Kind;
using telemetry::Metric;
using telemetry::MetricStatus;
using telemetry::Reading;
using telemetry::Unavailable;

TYPED_TEST_P(SourceContract, NothingIsSampledBeforeTheProbe) {
  const auto source = TypeParam::make();
  std::vector<Reading> readings;
  source->sample(readings);
  EXPECT_TRUE(readings.empty());
}

TYPED_TEST_P(SourceContract, TheProbeIsNeverSilentAndEveryAbsenceCarriesARemedy) {
  const auto source = TypeParam::make();
  const std::vector<MetricStatus> statuses = source->probe();
  ASSERT_FALSE(statuses.empty()) << "a silently empty column is the worst outcome (F3)";
  const std::string prefix = std::string(source->name()) + ".";
  std::vector<std::tuple<std::string, telemetry::Scope, std::uint32_t>> seen;
  for (const MetricStatus& status : statuses) {
    EXPECT_TRUE(telemetry::is_valid_metric_name(status.metric.name)) << status.metric.name;
    EXPECT_EQ(status.metric.name.rfind(prefix, 0), 0U) << status.metric.name;
    EXPECT_FALSE(status.metric.provider.empty()) << status.metric.name;
    if (status.metric.kind == Kind::kGauge) {
      EXPECT_FALSE(status.metric.modulus.has_value()) << "a gauge does not wrap";
    }
    if (status.unavailable.has_value()) {
      EXPECT_NE(status.unavailable->kind, topology::Availability::kPresent);
      EXPECT_FALSE(status.unavailable->remediation.empty()) << status.metric.name;
      EXPECT_FALSE(status.unavailable->detail.empty()) << status.metric.name;
    }
    const auto identity =
        std::make_tuple(status.metric.name, status.metric.scope, status.metric.scope_index);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), identity), 0) << "two statuses, one series";
    seen.push_back(identity);
  }
}

TYPED_TEST_P(SourceContract, EachRoundReadsEachPresentMetricOnceInProbeOrder) {
  const auto source = TypeParam::make();
  std::vector<Metric> present;
  for (const MetricStatus& status : source->probe()) {
    if (!status.unavailable.has_value()) {
      present.push_back(status.metric);
    }
  }
  for (int round = 0; round < TypeParam::kRounds; ++round) {
    std::vector<Reading> readings = {Gauge{-1.0}};  // sample() appends; it does not clear
    source->sample(readings);
    ASSERT_EQ(readings.size(), present.size() + 1) << "round " << round;
    for (std::size_t at = 0; at < present.size(); ++at) {
      const Reading& reading = readings[at + 1];
      const Metric& metric = present[at];
      if (const auto* gauge = std::get_if<Gauge>(&reading)) {
        EXPECT_EQ(metric.kind, Kind::kGauge) << metric.name;
        EXPECT_TRUE(std::isfinite(gauge->value)) << metric.name;
      } else if (const auto* counter = std::get_if<Counter>(&reading)) {
        EXPECT_EQ(metric.kind, Kind::kCounter) << metric.name;
        if (metric.modulus.has_value()) {
          EXPECT_LT(counter->raw, *metric.modulus) << metric.name;
        }
      } else {
        const auto& unavailable = std::get<Unavailable>(reading);
        EXPECT_NE(unavailable.kind, topology::Availability::kPresent) << metric.name;
        EXPECT_NE(unavailable.kind, topology::Availability::kAbsent)
            << metric.name << ": present at the probe, so gone now, not absent";
        EXPECT_FALSE(unavailable.remediation.empty()) << metric.name;
      }
    }
  }
}

REGISTER_TYPED_TEST_SUITE_P(SourceContract, NothingIsSampledBeforeTheProbe,
                            TheProbeIsNeverSilentAndEveryAbsenceCarriesARemedy,
                            EachRoundReadsEachPresentMetricOnceInProbeOrder);

namespace timeline_suite {

inline Metric gauge(const std::string& name, std::uint32_t index = 0) {
  return Metric{name,         telemetry::Unit::kCelsius,
                Kind::kGauge, telemetry::Scope::kPackage,
                index,        std::nullopt,
                "test"};
}

inline Metric counter(const std::string& name) {
  return Metric{name, telemetry::Unit::kJoules, Kind::kCounter, telemetry::Scope::kPackage,
                0,    std::uint64_t{1} << 32U,  "test"};
}

}  // namespace timeline_suite

template <typename Subject>
class TimelineContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(TimelineContract);

using telemetry::Point;
using telemetry::TimelineFault;

TYPED_TEST_P(TimelineContract, SeriesAreNumberedInOrderAndDuplicatesRefused) {
  const auto timeline = TypeParam::make(100);
  EXPECT_EQ(timeline->series_count(), 0U);
  EXPECT_EQ(timeline->add_series(timeline_suite::gauge("thermal.package")).value(), 0U);
  EXPECT_EQ(timeline->add_series(timeline_suite::gauge("thermal.package", 1)).value(), 1U);
  EXPECT_EQ(timeline->add_series(timeline_suite::counter("power.package")).value(), 2U);
  Metric again = timeline_suite::gauge("thermal.package", 1);
  again.provider = "another";
  const auto duplicate = timeline->add_series(again);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().fault, TimelineFault::kDuplicateSeries);
  EXPECT_EQ(timeline->series_count(), 3U);
  EXPECT_EQ(timeline->metric(2), timeline_suite::counter("power.package"));
}

TYPED_TEST_P(TimelineContract, PointsComeBackInOrderAndOnlyLaterOnesAreAccepted) {
  const auto timeline = TypeParam::make(100);
  const auto id = timeline->add_series(timeline_suite::gauge("thermal.package")).value();
  ASSERT_TRUE(timeline->append(id, Point{100, Gauge{40.0}}).has_value());
  ASSERT_TRUE(timeline
                  ->append(id, Point{200, Unavailable{topology::Availability::kVanished, "/sys/x",
                                                      "gone", "re-plug"}})
                  .has_value());
  ASSERT_TRUE(timeline->append(id, Point{300, Gauge{41.5}}).has_value());
  for (const std::int64_t stale : {std::int64_t{300}, std::int64_t{250}}) {
    const auto refused = timeline->append(id, Point{stale, Gauge{1.0}});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().fault, TimelineFault::kOutOfOrder);
  }
  const auto points = timeline->points(id);
  ASSERT_EQ(points.size(), 3U) << "a refusal leaves the timeline as it was";
  EXPECT_EQ(points[0], (Point{100, Gauge{40.0}}));
  EXPECT_TRUE(std::holds_alternative<Unavailable>(points[1].reading));
  EXPECT_EQ(points[2].timestamp_ns, 300);
}

TYPED_TEST_P(TimelineContract, SeriesAreIndependentInTime) {
  const auto timeline = TypeParam::make(100);
  const auto a = timeline->add_series(timeline_suite::gauge("thermal.a")).value();
  const auto b = timeline->add_series(timeline_suite::gauge("thermal.b")).value();
  ASSERT_TRUE(timeline->append(a, Point{500, Gauge{1.0}}).has_value());
  EXPECT_TRUE(timeline->append(b, Point{100, Gauge{2.0}}).has_value()) << "order is per series";
}

TYPED_TEST_P(TimelineContract, AReadingOfTheWrongKindOrForNoSeriesIsRefused) {
  const auto timeline = TypeParam::make(100);
  const auto gauge = timeline->add_series(timeline_suite::gauge("thermal.package")).value();
  const auto counter = timeline->add_series(timeline_suite::counter("power.package")).value();
  EXPECT_EQ(timeline->append(gauge, Point{1, Counter{5}}).error().fault,
            TimelineFault::kKindMismatch);
  EXPECT_EQ(timeline->append(counter, Point{1, Gauge{5.0}}).error().fault,
            TimelineFault::kKindMismatch);
  EXPECT_TRUE(timeline->append(counter, Point{1, Counter{5}}).has_value());
  EXPECT_EQ(timeline->append(7, Point{1, Gauge{5.0}}).error().fault, TimelineFault::kUnknownSeries);
  EXPECT_TRUE(timeline->points(gauge).empty());
}

TYPED_TEST_P(TimelineContract, CapacityIsRefusedNeverMadeByDroppingPoints) {
  const auto timeline = TypeParam::make(3);
  const auto a = timeline->add_series(timeline_suite::gauge("thermal.a")).value();
  const auto b = timeline->add_series(timeline_suite::gauge("thermal.b")).value();
  ASSERT_TRUE(timeline->append(a, Point{1, Gauge{1.0}}).has_value());
  ASSERT_TRUE(timeline->append(b, Point{1, Gauge{2.0}}).has_value());
  ASSERT_TRUE(timeline->append(a, Point{2, Gauge{3.0}}).has_value());
  const auto full = timeline->append(b, Point{2, Gauge{4.0}});
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error().fault, TimelineFault::kCapacity);
  EXPECT_EQ(timeline->points(a).size(), 2U);
  EXPECT_EQ(timeline->points(b).size(), 1U);
  EXPECT_EQ(timeline->points(a)[0], (Point{1, Gauge{1.0}})) << "the oldest point is kept";
}

TYPED_TEST_P(TimelineContract, PhasesAreMarkedInOrder) {
  const auto timeline = TypeParam::make(10);
  EXPECT_TRUE(timeline->phases().empty());
  ASSERT_TRUE(timeline->mark_phase({0, "settle"}).has_value());
  ASSERT_TRUE(timeline->mark_phase({10, "compression"}).has_value());
  EXPECT_EQ(timeline->mark_phase({10, "again"}).error().fault, TimelineFault::kOutOfOrder);
  ASSERT_EQ(timeline->phases().size(), 2U);
  EXPECT_EQ(timeline->phases()[1], (telemetry::PhaseMark{10, "compression"}));
}

REGISTER_TYPED_TEST_SUITE_P(TimelineContract, SeriesAreNumberedInOrderAndDuplicatesRefused,
                            PointsComeBackInOrderAndOnlyLaterOnesAreAccepted,
                            SeriesAreIndependentInTime,
                            AReadingOfTheWrongKindOrForNoSeriesIsRefused,
                            CapacityIsRefusedNeverMadeByDroppingPoints, PhasesAreMarkedInOrder);

}  // namespace loadforge::contracts

#endif
