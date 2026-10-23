// SPDX-License-Identifier: GPL-3.0-or-later
//
// The telemetry of a run, assembled: which sources, in what order, over which
// roots, and how big a timeline.

#include "main/telemetry_rig.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/fs.hpp"
#include "support/fake_syscalls.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::cli {
namespace {

TEST(TelemetryRigTest, TheMachinesRootsAreEmptyAndTheDefaultMountPoints) {
  const MachineRoots machine;
  EXPECT_EQ(machine.prefix, "");
  EXPECT_EQ(machine.sysfs, "/sys");
  EXPECT_EQ(machine.procfs, "/proc");
}

TEST(TelemetryRigTest, ALongRootIsKeptWholeInEachOfTheThree) {
  const std::string tree = "/a/directory/with/a/name/longer/than/any/small/string/buffer";
  const MachineRoots under = roots_under(tree);
  EXPECT_EQ(under.prefix, tree);
  EXPECT_EQ(under.sysfs, tree + "/sys");
  EXPECT_EQ(under.procfs, tree + "/proc");
}

TEST(TelemetryRigTest, ATestTreeHasItsOwnSysAndProc) {
  const MachineRoots under = roots_under("/tmp/tree");
  EXPECT_EQ(under.prefix, "/tmp/tree");
  EXPECT_EQ(under.sysfs, "/tmp/tree/sys");
  EXPECT_EQ(under.procfs, "/tmp/tree/proc");
}

TEST(TelemetryRigTest, EverySourceIsMadeOnceInTheOrderTheReportShowsThem) {
  testing::FakeSyscalls syscalls;
  platform::FileSystem files(syscalls);
  const auto sources = make_sources(syscalls, files, MachineRoots{});
  std::vector<std::string> names;
  names.reserve(sources.size());
  for (const auto& source : sources) {
    names.emplace_back(source->name());
  }
  EXPECT_EQ(names, (std::vector<std::string>{"thermal", "fan", "frequency", "memory", "power",
                                             "counters", "edac"}));
}

TEST(TelemetryRigTest, ARootMovesEverySourceToTheTree) {
  // A tree with only a memory file under its own root: the memory source finds
  // it there and nowhere else.
  testing::FakeSyscalls syscalls;
  syscalls.add_file("/tree/proc/meminfo", "MemTotal: 100 kB\nMemAvailable: 50 kB\n");
  platform::FileSystem files(syscalls);
  const auto rig = make_rig(syscalls, files, roots_under("/tree"), 1000);
  ASSERT_TRUE(rig.has_value());
  const telemetry::CapabilityReport& report = rig.value()->capabilities();
  ASSERT_EQ(report.sources.size(), 7U);
  EXPECT_EQ(report.sources[3].source, "memory");
  EXPECT_FALSE(report.sources[3].metrics.empty());
  bool total_present = false;
  for (const telemetry::MetricStatus& status : report.sources[3].metrics) {
    total_present =
        total_present || (status.metric.name == "memory.total" && !status.unavailable.has_value());
  }
  EXPECT_TRUE(total_present);
}

TEST(TelemetryRigTest, TheRigHoldsATimelineOfTheCapacityAskedAndTheSeriesTheProbeFound) {
  testing::FakeSyscalls syscalls;
  syscalls.add_file("/proc/meminfo", "MemTotal: 100 kB\n");
  platform::FileSystem files(syscalls);
  const auto rig = make_rig(syscalls, files, MachineRoots{}, 777);
  ASSERT_TRUE(rig.has_value());
  EXPECT_EQ(rig.value()->timeline().capacity(), 777U);
  EXPECT_GE(rig.value()->timeline().series_count(), 1U);
  EXPECT_EQ(rig.value()->capabilities().sources.size(), 7U);
}

telemetry::MetricStatus status_of(std::string name, telemetry::Unit unit, telemetry::Kind kind,
                                  std::optional<telemetry::Unavailable> why = std::nullopt) {
  telemetry::MetricStatus status;
  status.metric = telemetry::Metric{std::move(name), unit,  kind, telemetry::Scope::kPackage, 0,
                                    std::nullopt,    "mock"};
  status.unavailable = std::move(why);
  return status;
}

telemetry::Unavailable absent(std::string detail) {
  return telemetry::Unavailable{topology::Availability::kAbsent, "/sys", std::move(detail),
                                "nothing can be done"};
}

TEST(TelemetryRigTest, ATemperatureIsAPresentGaugeInDegreesCelsiusFromAnySource) {
  telemetry::CapabilityReport report;
  EXPECT_FALSE(has_temperature(report));
  report.sources.push_back({"thermal",
                            {status_of("thermal.package", telemetry::Unit::kCelsius,
                                       telemetry::Kind::kGauge, absent("no chip"))}});
  EXPECT_FALSE(has_temperature(report)) << "an absent sensor is not a temperature";
  report.sources.push_back(
      {"power", {status_of("power.package", telemetry::Unit::kWatts, telemetry::Kind::kGauge)}});
  EXPECT_FALSE(has_temperature(report)) << "a present gauge of watts is not";
  report.sources.push_back(
      {"odd", {status_of("odd.count", telemetry::Unit::kCelsius, telemetry::Kind::kCounter)}});
  EXPECT_FALSE(has_temperature(report)) << "nor a counter";
  report.sources.push_back(
      {"thermal", {status_of("thermal.core", telemetry::Unit::kCelsius, telemetry::Kind::kGauge)}});
  EXPECT_TRUE(has_temperature(report));
}

TEST(TelemetryRigTest, WhyNoTemperatureIsOfferedIsTheThermalSourcesOwnWordsAndNobodyElses) {
  telemetry::CapabilityReport report;
  EXPECT_EQ(unread_temperature(report), "");
  report.sources.push_back(
      {"fan",
       {status_of("fan.speed", telemetry::Unit::kRpm, telemetry::Kind::kGauge, absent("no fan"))}});
  EXPECT_EQ(unread_temperature(report), "") << "another source's absence is not the reason";
  report.sources.push_back(
      {"thermal",
       {status_of("thermal.package", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                  absent("no hwmon chip")),
        status_of("thermal.core", telemetry::Unit::kCelsius, telemetry::Kind::kGauge),
        status_of("thermal.zone", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                  absent("no zone"))}});
  EXPECT_EQ(unread_temperature(report),
            "thermal.package: no hwmon chip; nothing can be done | thermal.zone: no zone; "
            "nothing can be done");
}

TEST(TelemetryRigTest, ASeriesBoundGrowsWithTheCpus) {
  EXPECT_EQ(series_bound(0), 64U);
  EXPECT_EQ(series_bound(4), 96U);
  EXPECT_EQ(series_bound(256), 64U + (8U * 256U));
}

TEST(TelemetryRigTest, ACapacityIsTheRoundsOfTheRunAndAMarginTimesTheSeries) {
  constexpr std::int64_t kSecond = 1'000'000'000;
  // Ten seconds at one round a second is ten rounds, and eight for the
  // baseline, the deadline, the grace and the last.
  EXPECT_EQ(timeline_capacity({10 * kSecond, kSecond, 4}), 18U * 4U);
  EXPECT_EQ(timeline_capacity({10 * kSecond, kSecond / 10, 1}), 108U);
  EXPECT_EQ(timeline_capacity({0, kSecond, 5}), 8U * 5U);
}

TEST(TelemetryRigTest, AnIntervalOfZeroOrALengthBelowZeroDoesNotDivideByZeroOrGoNegative) {
  EXPECT_EQ(timeline_capacity({5, 0, 1}), 13U) << "an interval of zero is taken as a nanosecond";
  EXPECT_EQ(timeline_capacity({-5, 1, 1}), 8U) << "a negative run is no run";
}

TEST(TelemetryRigTest, ACapacityThatCannotBeHeldSaturates) {
  EXPECT_EQ(timeline_capacity({std::numeric_limits<std::int64_t>::max(), 1,
                               std::numeric_limits<std::size_t>::max()}),
            std::numeric_limits<std::size_t>::max());
}

}  // namespace
}  // namespace loadforge::cli
