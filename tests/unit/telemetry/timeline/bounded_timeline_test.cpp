// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bounded timeline: the contract's suite against it, then what the suite
// leaves open -- the text of each refusal, a refusal that changes nothing,
// and the memory a five-hour run costs.

#include "telemetry/timeline/bounded_timeline.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>

#include "contracts/telemetry/telemetry_contract.hpp"

namespace loadforge::telemetry {
namespace {

using contracts::timeline_suite::counter;
using contracts::timeline_suite::gauge;

struct BoundedSubject {
  static std::unique_ptr<Timeline> make(std::size_t capacity) {
    return std::make_unique<BoundedTimeline>(capacity);
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

INSTANTIATE_TYPED_TEST_SUITE_P(Bounded, TimelineContract,
                               ::testing::Types<telemetry::BoundedSubject>);
// This file provides the timeline and consumes no source; the source suite
// is instantiated by the sampler's tests, against its own mock.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(SourceContract);

}  // namespace loadforge::contracts

namespace loadforge::telemetry {
namespace {

Unavailable gone() {
  return Unavailable{topology::Availability::kVanished, "/sys/x", "gone", "re-plug"};
}

TEST(BoundedTimeline, AnUnavailablePointIsAcceptedForAGaugeAndForACounter) {
  BoundedTimeline timeline(10);
  const SeriesId g = timeline.add_series(gauge("thermal.package")).value();
  const SeriesId c = timeline.add_series(counter("power.package")).value();
  EXPECT_TRUE(timeline.append(g, Point{1, gone()}).has_value());
  EXPECT_TRUE(timeline.append(c, Point{1, gone()}).has_value());
  EXPECT_EQ(timeline.size(), 2U);
}

TEST(BoundedTimeline, EachRefusalNamesItsFaultAndWhatWasWrong) {
  BoundedTimeline timeline(2);
  const SeriesId g = timeline.add_series(gauge("thermal.package")).value();

  const auto duplicate = timeline.add_series(gauge("thermal.package"));
  EXPECT_EQ(duplicate.error().fault, TimelineFault::kDuplicateSeries);
  EXPECT_EQ(duplicate.error().detail, "a series named thermal.package already exists");

  const auto unknown = timeline.append(9, Point{1, Gauge{1.0}});
  EXPECT_EQ(unknown.error().fault, TimelineFault::kUnknownSeries);
  EXPECT_EQ(unknown.error().detail, "no series 9");

  ASSERT_TRUE(timeline.append(g, Point{5, Gauge{1.0}}).has_value());
  const auto stale = timeline.append(g, Point{5, Gauge{1.0}});
  EXPECT_EQ(stale.error().fault, TimelineFault::kOutOfOrder);
  EXPECT_EQ(stale.error().detail, "timestamp 5 is not later than 5");

  const auto wrong = timeline.append(g, Point{6, Counter{1}});
  EXPECT_EQ(wrong.error().fault, TimelineFault::kKindMismatch);
  EXPECT_EQ(wrong.error().detail, "the reading does not suit thermal.package");

  ASSERT_TRUE(timeline.append(g, Point{6, Gauge{2.0}}).has_value());
  const auto full = timeline.append(g, Point{7, Gauge{3.0}});
  EXPECT_EQ(full.error().fault, TimelineFault::kCapacity);
  EXPECT_EQ(full.error().detail, "the timeline holds 2 points");

  ASSERT_TRUE(timeline.mark_phase({10, "settle"}).has_value());
  const auto phase = timeline.mark_phase({4, "late"});
  EXPECT_EQ(phase.error().fault, TimelineFault::kOutOfOrder);
  EXPECT_EQ(phase.error().detail, "phase late starts at 4, not later than 10");
}

TEST(BoundedTimeline, ARefusedPointCountsAgainstNothing) {
  BoundedTimeline timeline(1);
  const SeriesId g = timeline.add_series(gauge("thermal.package")).value();
  EXPECT_FALSE(timeline.append(g, Point{1, Counter{1}}).has_value());
  EXPECT_FALSE(timeline.append(3, Point{1, Gauge{1.0}}).has_value());
  EXPECT_EQ(timeline.size(), 0U);
  EXPECT_TRUE(timeline.append(g, Point{1, Gauge{1.0}}).has_value()) << "the room was not spent";
}

TEST(BoundedTimeline, AZeroCapacityTimelineRefusesTheFirstPoint) {
  BoundedTimeline timeline(0);
  const SeriesId g = timeline.add_series(gauge("thermal.package")).value();
  EXPECT_EQ(timeline.append(g, Point{1, Gauge{1.0}}).error().fault, TimelineFault::kCapacity);
  EXPECT_EQ(timeline.capacity(), 0U);
}

TEST(BoundedTimeline, PhasesAreNotPoints) {
  BoundedTimeline timeline(0);
  EXPECT_TRUE(timeline.mark_phase({0, "idle"}).has_value());
  EXPECT_EQ(timeline.size(), 0U);
}

TEST(RunCapacity, IsSecondsTimesRateTimesSeries) {
  EXPECT_EQ(run_capacity(18000, 10, 64), 11520000U);
  EXPECT_EQ(run_capacity(0, 10, 64), 0U);
}

TEST(RunCapacity, SaturatesInsteadOfWrapping) {
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  EXPECT_EQ(run_capacity(kMax, 2, 1), kMax);
  EXPECT_EQ(run_capacity(kMax / 2 + 1, 1, 2), kMax);
  EXPECT_EQ(memory_bound_bytes(kMax), kMax);
  EXPECT_EQ(memory_bound_bytes(kMax / sizeof(Point) + 1), kMax);
}

// EXIT CRITERION 2. The stated bound for a five-hour run at 10 Hz over 64
// series is 3 * 11,520,000 * sizeof(Point) bytes; the test checks the
// arithmetic and then shows the bound is honest on a run one hundredth the
// size, by counting the room the buffers really hold.
TEST(MemoryBound, AFiveHourRunAtTenHertzOverSixtyFourSeriesIsStated) {
  const std::size_t points = run_capacity(std::size_t{5} * 3600, 10, 64);
  EXPECT_EQ(points, 11520000U);
  EXPECT_EQ(memory_bound_bytes(points), std::size_t{3} * std::size_t{11520000} * sizeof(Point));
  RecordProperty("point_bytes", static_cast<int>(sizeof(Point)));
  RecordProperty("five_hour_bound_mib", static_cast<int>(memory_bound_bytes(points) >> 20U));
}

TEST(MemoryBound, TheRoomHeldIsNeverMoreThanTwiceThePointsHeld) {
  const std::size_t points = run_capacity(180, 10, 64);  // a hundredth of five hours
  BoundedTimeline timeline(points);
  for (std::uint32_t at = 0; at < 64; ++at) {
    ASSERT_TRUE(timeline.add_series(gauge("thermal.sensor", at)).has_value());
  }
  for (std::int64_t round = 1; round <= 1800; ++round) {
    for (SeriesId id = 0; id < 64; ++id) {
      ASSERT_TRUE(timeline.append(id, Point{round, Gauge{1.0}}).has_value());
    }
    ASSERT_LE(timeline.reserved(), 2 * timeline.size());
  }
  EXPECT_EQ(timeline.size(), points);
  EXPECT_LE(timeline.reserved() * sizeof(Point), memory_bound_bytes(points));
  EXPECT_EQ(timeline.append(0, Point{1801, Gauge{1.0}}).error().fault, TimelineFault::kCapacity);
}

}  // namespace
}  // namespace loadforge::telemetry
