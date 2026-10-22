// SPDX-License-Identifier: GPL-3.0-or-later
//
// The temperature ceiling's eyes (controller/temperature.hpp): which series of
// the timeline are temperatures, what a round showed, and what each way of
// stopping a run says -- over this module's own timeline mock
// (telemetry_mocks.hpp) and a plain view that can hold what a timeline refuses.

#include "controller/temperature.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "telemetry/sample.hpp"
#include "telemetry/timeline.hpp"
#include "topology/source.hpp"
#include "unit/controller/telemetry_mocks.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kS = 1'000'000'000;

telemetry::Metric celsius(std::string name, telemetry::Scope scope, std::uint32_t index,
                          std::string provider = "hwmon:mock") {
  return telemetry::Metric{
      std::move(name), telemetry::Unit::kCelsius, telemetry::Kind::kGauge, scope, index,
      std::nullopt,    std::move(provider)};
}

telemetry::Reading gone(std::string detail) {
  return telemetry::Unavailable{topology::Availability::kUnreadable, "/sys/x", std::move(detail),
                                "check the kernel log"};
}

/// A timeline with the given series, to which a round is added by reading.
class Board {
 public:
  explicit Board(std::vector<telemetry::Metric> metrics) {
    for (const telemetry::Metric& metric : metrics) {
      ids.push_back(timeline.add_series(metric).value());
    }
  }
  void round(std::int64_t at_ns, const std::vector<telemetry::Reading>& readings) {
    for (std::size_t at = 0; at < readings.size(); ++at) {
      ASSERT_TRUE(timeline.append(ids[at], {at_ns, readings[at]}).has_value());
    }
  }
  mocks::MockTimeline timeline;
  std::vector<telemetry::SeriesId> ids;
};

/// A view that holds any point, as a timeline never will.
class PlainView final : public telemetry::TimelineView {
 public:
  [[nodiscard]] std::size_t series_count() const noexcept override { return 1; }
  [[nodiscard]] const telemetry::Metric& metric(telemetry::SeriesId /*series*/) const override {
    return metric_;
  }
  [[nodiscard]] std::span<const telemetry::Point> points(
      telemetry::SeriesId /*series*/) const override {
    return points_;
  }
  [[nodiscard]] std::span<const telemetry::PhaseMark> phases() const override { return {}; }
  telemetry::Metric metric_ = celsius("thermal.package", telemetry::Scope::kPackage, 0);
  std::vector<telemetry::Point> points_;
};

TEST(TemperatureWatchTest, OnlyGaugesInDegreesCelsiusAreTemperatures) {
  Board board(
      {celsius("thermal.package", telemetry::Scope::kPackage, 0, "hwmon:coretemp"),
       telemetry::Metric{"power.package", telemetry::Unit::kWatts, telemetry::Kind::kGauge,
                         telemetry::Scope::kPackage, 0, std::nullopt, "powercap"},
       telemetry::Metric{"edac.corrected", telemetry::Unit::kCelsius, telemetry::Kind::kCounter,
                         telemetry::Scope::kMachine, 0, std::nullopt, "edac"},
       celsius("thermal.machine", telemetry::Scope::kMachine, 0, "mock")});
  board.round(
      1, {telemetry::Gauge{50}, telemetry::Gauge{20}, telemetry::Counter{1}, telemetry::Gauge{40}});
  const TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);
  ASSERT_EQ(watch.sensors().size(), 2U);
  EXPECT_EQ(watch.sensors()[0].series, 0U);
  EXPECT_EQ(watch.sensors()[0].metric, "thermal.package");
  EXPECT_EQ(watch.sensors()[0].name, "thermal.package 0");
  EXPECT_EQ(watch.sensors()[0].provider, "hwmon:coretemp");
  EXPECT_EQ(watch.sensors()[1].series, 3U);
  EXPECT_EQ(watch.sensors()[1].name, "thermal.machine") << "no index for a machine-wide sensor";
  EXPECT_EQ(watch.readable(), 2U);
  EXPECT_FALSE(watch.hot_at_start().has_value());
}

TEST(TemperatureWatchTest, NoSensorAtAllIsAWatchOfNothing) {
  Board board({telemetry::Metric{"power.package", telemetry::Unit::kWatts, telemetry::Kind::kGauge,
                                 telemetry::Scope::kPackage, 0, std::nullopt, "powercap"}});
  board.round(1, {telemetry::Gauge{20}});
  const TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);
  EXPECT_TRUE(watch.sensors().empty());
  EXPECT_EQ(watch.readable(), 0U);
  EXPECT_EQ(watch.describe_sensors(), "");
}

TEST(TemperatureWatchTest, TheSensorsAreNamedOnceEachAndCountedWhereAFamilyHasSeveral) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0),
               celsius("thermal.core", telemetry::Scope::kCore, 0),
               celsius("thermal.zone", telemetry::Scope::kDevice, 3),
               celsius("thermal.core", telemetry::Scope::kCore, 1),
               celsius("thermal.core", telemetry::Scope::kCore, 2)});
  board.round(1, {telemetry::Gauge{1}, telemetry::Gauge{1}, telemetry::Gauge{1},
                  telemetry::Gauge{1}, telemetry::Gauge{1}});
  const TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);
  EXPECT_EQ(watch.describe_sensors(), "thermal.package 0, thermal.core x3, thermal.zone 3");
}

TEST(TemperatureWatchTest, AReadingBelowAtAndAboveTheCeilingIsToldApart) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0, "hwmon:coretemp")});
  board.round(1, {telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 85, 10 * kS, 1);

  board.round(2, {telemetry::Gauge{84.9}});
  Observation below = watch.observe(2, 2 * kS);
  EXPECT_FALSE(below.stop.has_value());
  EXPECT_TRUE(below.notes.empty());

  board.round(3, {telemetry::Gauge{85.0}});
  Observation at = watch.observe(3, 3 * kS);
  ASSERT_TRUE(at.stop.has_value()) << "at the ceiling is a stop";
  EXPECT_EQ(at.stop->cause, StopCause::kReached);
  EXPECT_EQ(at.stop->sensor, "thermal.package 0 (hwmon:coretemp)");
  EXPECT_EQ(at.stop->celsius, 85.0);
  EXPECT_EQ(at.stop->ceiling_c, 85);
  EXPECT_EQ(at.stop->at_ns, 3 * kS);

  board.round(4, {telemetry::Gauge{91.25}});
  Observation above = watch.observe(4, 4 * kS);
  ASSERT_TRUE(above.stop.has_value());
  EXPECT_EQ(above.stop->celsius, 91.25);
  EXPECT_EQ(describe_reading(*above.stop),
            "thermal.package 0 (hwmon:coretemp) read 91.2 C, at or above the ceiling of 85 C");
  EXPECT_EQ(describe(*above.stop),
            "thermal.package 0 (hwmon:coretemp) read 91.2 C, at or above the ceiling of 85 C, "
            "4.000 s into the run");
}

TEST(TemperatureWatchTest, OfSeveralSensorsTheOneThatCrossesIsNamedAndTheHottestOfThoseThatDo) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0),
               celsius("thermal.core", telemetry::Scope::kCore, 0),
               celsius("thermal.core", telemetry::Scope::kCore, 1),
               celsius("thermal.zone", telemetry::Scope::kDevice, 0)});
  board.round(
      1, {telemetry::Gauge{40}, telemetry::Gauge{40}, telemetry::Gauge{40}, telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);

  board.round(2, {telemetry::Gauge{60}, telemetry::Gauge{89.9}, telemetry::Gauge{91},
                  telemetry::Gauge{70}});
  Observation one = watch.observe(2, kS);
  ASSERT_TRUE(one.stop.has_value());
  EXPECT_EQ(one.stop->sensor, "thermal.core 1 (hwmon:mock)") << "only core 1 is at 90";

  board.round(
      3, {telemetry::Gauge{92}, telemetry::Gauge{95}, telemetry::Gauge{91}, telemetry::Gauge{95}});
  Observation several = watch.observe(3, 2 * kS);
  ASSERT_TRUE(several.stop.has_value());
  EXPECT_EQ(several.stop->sensor, "thermal.core 0 (hwmon:mock)")
      << "of two at 95 the first, and not the hottest-last";
  EXPECT_EQ(several.stop->celsius, 95.0);

  board.round(
      4, {telemetry::Gauge{97}, telemetry::Gauge{91}, telemetry::Gauge{91}, telemetry::Gauge{91}});
  Observation first_is_hottest = watch.observe(4, 3 * kS);
  ASSERT_TRUE(first_is_hottest.stop.has_value());
  EXPECT_EQ(first_is_hottest.stop->sensor, "thermal.package 0 (hwmon:mock)");
  EXPECT_EQ(first_is_hottest.stop->celsius, 97.0);
}

TEST(TemperatureWatchTest, AMachineAlreadyAtTheCeilingBeforeTheFirstWorkerIsToldAtTheStart) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0)});
  board.round(5, {telemetry::Gauge{90}});
  const TemperatureWatch watch(board.timeline, 90, 10 * kS, 5);
  ASSERT_TRUE(watch.hot_at_start().has_value());
  EXPECT_EQ(watch.hot_at_start()->at_ns, 0);
  EXPECT_EQ(watch.hot_at_start()->celsius, 90.0);
}

TEST(TemperatureWatchTest, ASensorThatStopsAnsweringIsToldOnceAndTheOthersAreStillWatched) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0, "hwmon:coretemp"),
               celsius("thermal.core", telemetry::Scope::kCore, 0, "hwmon:coretemp")});
  board.round(1, {telemetry::Gauge{40}, telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);
  EXPECT_EQ(watch.readable(), 2U);

  board.round(2, {gone("the sensor disappeared"), telemetry::Gauge{50}});
  Observation lost = watch.observe(2, kS);
  EXPECT_FALSE(lost.stop.has_value());
  ASSERT_EQ(lost.notes.size(), 1U);
  EXPECT_EQ(lost.notes[0],
            "thermal.package 0 (hwmon:coretemp) can no longer be read: the sensor disappeared");
  EXPECT_EQ(watch.readable(), 1U);
  EXPECT_EQ(watch.last_reason(), "the sensor disappeared");

  board.round(3, {gone("the sensor disappeared"), telemetry::Gauge{93}});
  Observation still = watch.observe(3, 2 * kS);
  EXPECT_TRUE(still.notes.empty()) << "told once";
  ASSERT_TRUE(still.stop.has_value()) << "the sensor that remains is still watched";
  EXPECT_EQ(still.stop->sensor, "thermal.core 0 (hwmon:coretemp)");

  board.round(4, {telemetry::Gauge{60}, telemetry::Gauge{60}});
  Observation back = watch.observe(4, 3 * kS);
  EXPECT_FALSE(back.stop.has_value());
  ASSERT_EQ(back.notes.size(), 1U);
  EXPECT_EQ(back.notes[0], "thermal.package 0 (hwmon:coretemp) can be read again");
  EXPECT_EQ(watch.readable(), 2U);
}

TEST(TemperatureWatchTest, ASensorThatCannotBeReadAtTheBaselineIsUnreadableAndSaysNothingAtFirst) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0),
               celsius("thermal.core", telemetry::Scope::kCore, 0)});
  board.round(1, {gone("denied"), telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 10 * kS, 1);
  EXPECT_EQ(watch.readable(), 1U);
  EXPECT_EQ(watch.sensors().size(), 2U) << "an unreadable sensor is still one that is watched";
  board.round(2, {gone("denied"), telemetry::Gauge{40}});
  EXPECT_TRUE(watch.observe(2, kS).notes.empty());
}

TEST(TemperatureWatchTest, NoSensorReadForLongerThanTheAllowanceMeansTheCeilingIsNotEnforced) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0)});
  board.round(10 * kS, {telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 5 * kS, 10 * kS);

  board.round(12 * kS, {gone("driver unloaded")});
  Observation lost = watch.observe(12 * kS, 2 * kS);
  EXPECT_FALSE(lost.stop.has_value()) << "2 s of 5 s";
  EXPECT_EQ(lost.notes.size(), 1U);

  board.round(15 * kS, {gone("driver unloaded")});
  EXPECT_FALSE(watch.observe(15 * kS, 5 * kS).stop.has_value())
      << "exactly the allowance is not past it";

  board.round(16 * kS, {gone("driver unloaded")});
  Observation blind = watch.observe(16 * kS, 6 * kS);
  ASSERT_TRUE(blind.stop.has_value());
  EXPECT_EQ(blind.stop->cause, StopCause::kUnwatched);
  EXPECT_EQ(blind.stop->blind_ns, 6 * kS);
  EXPECT_EQ(blind.stop->detail, "driver unloaded");
  EXPECT_EQ(blind.stop->at_ns, 6 * kS);
  EXPECT_EQ(describe(*blind.stop),
            "no temperature could be read for 6.0 s (driver unloaded), so the ceiling of 90 C "
            "can no longer be enforced, 6.000 s into the run");
}

TEST(TemperatureWatchTest, ASensorThatAnswersAgainRestartsTheAllowance) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0)});
  board.round(0, {telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 5 * kS, 0);
  board.round(4 * kS, {gone("x")});
  EXPECT_FALSE(watch.observe(4 * kS, 4 * kS).stop.has_value());
  board.round(5 * kS, {telemetry::Gauge{41}});
  EXPECT_FALSE(watch.observe(5 * kS, 5 * kS).stop.has_value());
  board.round(9 * kS, {gone("x")});
  EXPECT_FALSE(watch.observe(9 * kS, 9 * kS).stop.has_value()) << "4 s since the last answer";
}

TEST(TemperatureWatchTest, ASeriesThatMissedTheRoundHasNoReadingInIt) {
  Board board({celsius("thermal.package", telemetry::Scope::kPackage, 0)});
  board.round(1, {telemetry::Gauge{40}});
  TemperatureWatch watch(board.timeline, 90, 1 * kS, 1);
  // No point is appended for round 2.
  Observation missed = watch.observe(2, kS);
  ASSERT_EQ(missed.notes.size(), 1U);
  EXPECT_EQ(missed.notes[0],
            "thermal.package 0 (hwmon:mock) can no longer be read: no reading was taken");
  // A series with no point at all is the same.
  Board empty({celsius("thermal.package", telemetry::Scope::kPackage, 0)});
  TemperatureWatch never(empty.timeline, 90, 1 * kS, 1);
  EXPECT_EQ(never.readable(), 0U);
}

TEST(TemperatureWatchTest, AReadingThatIsNeitherAGaugeNorAnUnavailableIsNotATemperature) {
  PlainView view;
  view.points_.push_back({1, telemetry::Counter{7}});
  const TemperatureWatch watch(view, 90, kS, 1);
  EXPECT_EQ(watch.readable(), 0U);
  EXPECT_EQ(watch.last_reason(), "the reading is not a temperature");
}

TEST(TemperatureStopTest, EveryFieldTakesPartInTheComparison) {
  TemperatureStop base;
  base.sensor = "a";
  base.celsius = 1.0;
  base.ceiling_c = 2;
  base.at_ns = 3;
  base.detail = "d";
  base.blind_ns = 4;
  EXPECT_EQ(base, TemperatureStop{base});
  const auto differs = [&base](auto change) {
    TemperatureStop other = base;
    change(other);
    return other == base;
  };
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.cause = StopCause::kUnwatched; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.sensor = "b"; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.celsius = 9.0; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.ceiling_c = 9; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.at_ns = 9; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.detail = "e"; }));
  EXPECT_FALSE(differs([](TemperatureStop& s) { s.blind_ns = 9; }));
  const TemperatureStop defaults;
  EXPECT_EQ(defaults.cause, StopCause::kReached);
  EXPECT_EQ(defaults.celsius, 0.0);
}

}  // namespace
}  // namespace loadforge::controller
