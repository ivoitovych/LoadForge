// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_TELEMETRY_TIMELINE_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_TELEMETRY_TIMELINE_CONTRACT_HPP

// Provider-side suite for src/telemetry/timeline.hpp (ADR 0003, 0004).
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

using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Kind;
using telemetry::Metric;
using telemetry::Unavailable;

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
