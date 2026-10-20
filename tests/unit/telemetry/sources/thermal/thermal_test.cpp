// SPDX-License-Identifier: GPL-3.0-or-later
//
// The thermal source: temperatures from hwmon and the thermal zones.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The six states (present, absent, denied, malformed, vanished mid-run, and --
// for a counter -- wrapped; temperatures are gauges, so the last does not
// apply) are put on the source through the substituted platform seam
// (fixtures::MemFs), over trees captured from this machine where it had
// anything (tests/fixtures/sysfs/thermal/absent) and built in the kernel's
// documented layout where it had not (hwmon_tree.hpp).

#include "telemetry/sources/thermal/thermal.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "contracts/telemetry/telemetry_contract.hpp"
#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/thermal/channel.hpp"
#include "telemetry/sources/thermal/hwmon.hpp"
#include "topology/source.hpp"
#include "unit/telemetry/sources/thermal/hwmon_tree.hpp"
#include "unit/telemetry/sources/thermal/owned.hpp"

namespace loadforge::telemetry {
namespace {

using fixtures::add_chip;
using fixtures::add_temp;
using fixtures::add_zone;
using fixtures::MemFs;
using fixtures::OwnedSource;
using topology::Availability;

const MetricStatus* find(const std::vector<MetricStatus>& statuses, std::string_view name,
                         std::uint32_t index) {
  for (const MetricStatus& status : statuses) {
    if (status.metric.name == name && status.metric.scope_index == index) {
      return &status;
    }
  }
  return nullptr;
}

std::vector<Reading> sample(Source& source) {
  std::vector<Reading> readings;
  source.sample(readings);
  return readings;
}

double gauge_of(const Reading& reading) {
  const auto* gauge = std::get_if<Gauge>(&reading);
  EXPECT_NE(gauge, nullptr);
  return gauge == nullptr ? -1.0 : gauge->value;
}

const Unavailable& unavailable_of(const Reading& reading) {
  const auto* unavailable = std::get_if<Unavailable>(&reading);
  EXPECT_NE(unavailable, nullptr);
  static const Unavailable none;
  return unavailable == nullptr ? none : *unavailable;
}

class ThermalTest : public ::testing::Test {
 protected:
  MemFs fs;
  platform::FileSystem files{fs};
  ThermalSource source{files};
};

// --- the machine this was written on: nothing to read ------------------------------------

TEST_F(ThermalTest, TheCapturedMachineHasNoSensorAndSaysSoWithARemedy) {
  fs.load(fixtures::fixture("sysfs/thermal/absent"));
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.package");
  ASSERT_TRUE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[0].unavailable->path, "/sys/class/hwmon");
  EXPECT_NE(statuses[0].unavailable->remediation.find("virtual machine"), std::string::npos);
  EXPECT_NE(statuses[0].unavailable->remediation.find("coretemp"), std::string::npos)
      << "the remedy names the driver that would supply it";
  EXPECT_TRUE(sample(source).empty()) << "an absent metric is not sampled, and never read as zero";
}

TEST_F(ThermalTest, ARootPrefixesEveryPath) {
  ThermalSource rooted(files, "/mnt/machine");
  fs.set("/mnt/machine/sys/class/hwmon/hwmon0/name", "coretemp\n");
  fs.set("/mnt/machine/sys/class/hwmon/hwmon0/temp1_input", "50000\n");
  fs.set("/mnt/machine/sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
  const std::vector<MetricStatus> statuses = rooted.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.package");
  EXPECT_FALSE(statuses[0].unavailable.has_value());

  ThermalSource empty(files, "/mnt/nothing");
  EXPECT_EQ(empty.probe()[0].unavailable->path, "/mnt/nothing/sys/class/hwmon");
}

// --- present: what each driver says an input is ----------------------------------------------

TEST_F(ThermalTest, CoretempNamesThePackageAndEveryCore) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "52000", "Package id 0");
  add_temp(fs, 0, 2, "48500", "Core 0");
  add_temp(fs, 0, 3, "43700", "Core 1");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);

  const MetricStatus* package = find(statuses, "thermal.package", 0);
  ASSERT_NE(package, nullptr);
  EXPECT_EQ(package->metric.unit, Unit::kCelsius);
  EXPECT_EQ(package->metric.kind, Kind::kGauge);
  EXPECT_EQ(package->metric.scope, Scope::kPackage);
  EXPECT_EQ(package->metric.provider, "hwmon:coretemp");
  EXPECT_FALSE(package->unavailable.has_value());
  const MetricStatus* core1 = find(statuses, "thermal.core", 1);
  ASSERT_NE(core1, nullptr);
  EXPECT_EQ(core1->metric.scope, Scope::kCore);
  EXPECT_NE(find(statuses, "thermal.core", 0), nullptr);

  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 3U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[0]), 52.0);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 48.5);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), 43.7) << "divided, so the nearest double to 43.7";
}

TEST_F(ThermalTest, K10tempTdieIsThePackageAndOtherInputsAreNotGuessedAt) {
  add_chip(fs, 0, "k10temp");
  add_temp(fs, 0, 1, "61000", "Tctl");
  add_temp(fs, 0, 2, "58000", "Tdie");
  add_temp(fs, 0, 3, "55000", "Tccd1");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);
  const MetricStatus* package = find(statuses, "thermal.package", 0);
  ASSERT_NE(package, nullptr);
  EXPECT_EQ(package->metric.provider, "hwmon:k10temp");
  EXPECT_EQ(std::count_if(statuses.begin(), statuses.end(),
                          [](const MetricStatus& s) { return s.metric.name == "thermal.sensor"; }),
            2)
      << "Tctl and Tccd1 are reported, as sensors, and neither is called the package";
  const std::vector<Reading> readings = sample(source);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 58.0) << "Tdie, whatever order the inputs came in";
}

TEST_F(ThermalTest, AnInputOfAnUnknownDriverIsASensorOfThatChipAndInput) {
  add_chip(fs, 0, "coretemp");
  add_chip(fs, 3, "nct6775");
  add_temp(fs, 3, 1, "35000");
  add_temp(fs, 3, 2, "36000", "SYSTIN");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.sensor");
  EXPECT_EQ(statuses[0].metric.provider, "hwmon:nct6775");
  EXPECT_EQ(statuses[0].metric.scope, Scope::kDevice);
  EXPECT_EQ(statuses[0].metric.scope_index, (3U << 8U) | 1U) << "chip 3, input 1";
  EXPECT_EQ(statuses[1].metric.scope_index, (3U << 8U) | 2U);
}

TEST_F(ThermalTest, EveryLabelThatIsNotExactlyTheDriversIsASensor) {
  struct Case {
    std::string chip;
    std::string label;
    std::string name;
    Scope scope;
    std::uint32_t index;
  };
  const Case cases[] = {
      {"coretemp", "Package id 1", "thermal.package", Scope::kPackage, 1},
      {"coretemp", "Core 7", "thermal.core", Scope::kCore, 7},
      {"coretemp", "Core 4294967295", "thermal.core", Scope::kCore, 4294967295U},
      {"coretemp", "Core 4294967296", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "Core ", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "Core x", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "Core -1", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "core 0", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "Package id", "thermal.sensor", Scope::kDevice, 1},
      {"coretemp", "Physical id 0", "thermal.sensor", Scope::kDevice, 1},
      {"k10temp", "Tdie", "thermal.package", Scope::kPackage, 0},
      {"k10temp", "Tdie2", "thermal.sensor", Scope::kDevice, 1},
      {"k10temp", "Package id 0", "thermal.sensor", Scope::kDevice, 1},
      {"acpitz", "Package id 0", "thermal.sensor", Scope::kDevice, 1},
  };
  for (const Case& c : cases) {
    MemFs one;
    platform::FileSystem one_files(one);
    ThermalSource probing(one_files);
    add_chip(one, 0, c.chip);
    add_temp(one, 0, 1, "40000", c.label);
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 1U) << c.chip << " / " << c.label;
    EXPECT_EQ(statuses[0].metric.name, c.name) << c.chip << " / " << c.label;
    EXPECT_EQ(statuses[0].metric.scope, c.scope) << c.chip << " / " << c.label;
    EXPECT_EQ(statuses[0].metric.scope_index, c.name == "thermal.sensor" ? ((0U << 8U) | 1U) : c.index)
        << c.chip << " / " << c.label;
  }
}

TEST_F(ThermalTest, TwoInputsClaimingOneSeriesAreBothReportedTheSecondAsASensor) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  add_chip(fs, 1, "coretemp");
  add_temp(fs, 1, 1, "51000", "Package id 0");
  add_temp(fs, 1, 2, "52000", "Core 0");
  add_temp(fs, 0, 2, "49000", "Core 0");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 4U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.package");
  EXPECT_EQ(statuses[1].metric.name, "thermal.core");
  EXPECT_EQ(statuses[2].metric.name, "thermal.sensor") << "the second package, not merged";
  EXPECT_EQ(statuses[2].metric.scope_index, (1U << 8U) | 1U);
  EXPECT_EQ(statuses[3].metric.name, "thermal.sensor") << "the second Core 0";
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 4U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), 51.0) << "each reads its own file";
}

TEST_F(ThermalTest, ThermalZonesAreReportedBesideHwmonNotMergedWithIt) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  add_zone(fs, 0, "x86_pkg_temp", "50000");
  add_zone(fs, 2, "", "30500");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);
  const MetricStatus* zone0 = find(statuses, "thermal.zone", 0);
  ASSERT_NE(zone0, nullptr);
  EXPECT_EQ(zone0->metric.provider, "thermal_zone:x86_pkg_temp");
  const MetricStatus* zone2 = find(statuses, "thermal.zone", 2);
  ASSERT_NE(zone2, nullptr);
  EXPECT_EQ(zone2->metric.provider, "thermal_zone") << "a zone with no type is still a zone";
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 3U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), 30.5);
}

TEST_F(ThermalTest, ANegativeTemperatureIsAReadingBelowAbsoluteZeroIsNot) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "-40000");
  add_temp(fs, 0, 2, "-273150");
  add_temp(fs, 0, 3, "-273151");
  add_temp(fs, 0, 4, "1000000");
  add_temp(fs, 0, 5, "1000001");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 5U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_FALSE(statuses[1].unavailable.has_value());
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
  EXPECT_FALSE(statuses[3].unavailable.has_value());
  EXPECT_EQ(statuses[4].unavailable->kind, Availability::kMalformed);
}

// --- discovery: what is looked at, and what is not a finding -------------------------------------

TEST_F(ThermalTest, InputsAreFoundWhereverTheyAreNumberedUpTo255) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "30000");
  add_temp(fs, 0, 9, "31000");
  add_temp(fs, 0, 255, "32000");
  add_temp(fs, 0, 256, "33000");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U) << "a gap is not a finding, and temp256 is past the bound";
  EXPECT_EQ(statuses[2].metric.scope_index, 255U);
}

TEST_F(ThermalTest, ChipsAreFoundFromHwmon0ToHwmon63) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "30000");
  add_chip(fs, 63, "nct6775");
  add_temp(fs, 63, 1, "31000");
  add_chip(fs, 64, "nct6775");
  add_temp(fs, 64, 1, "32000");
  EXPECT_EQ(source.probe().size(), 2U);
  EXPECT_EQ(kMaxHwmonChips, 64U);
}

TEST_F(ThermalTest, AChipWithNoNameFileIsNotThere) {
  fs.set(fixtures::hwmon_dir(0) + "/temp1_input", "30000\n");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent) << "the 'no sensor' answer";
}

TEST_F(ThermalTest, AChipWhoseNameCannotBeReadIsKeptUnnamedAndItsSensorsAreUnidentified) {
  for (const int error : {EACCES, EIO}) {
    MemFs one;
    platform::FileSystem one_files(one);
    ThermalSource probing(one_files);
    add_chip(one, 2, "coretemp");
    add_temp(one, 2, 1, "50000", "Package id 0");
    if (error == EACCES) {
      one.fail_open(fixtures::hwmon_dir(2) + "/name", error);
    } else {
      one.fail_read(fixtures::hwmon_dir(2) + "/name", error);
    }
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 1U) << "errno " << error;
    EXPECT_EQ(statuses[0].metric.name, "thermal.sensor") << "nothing is identified by a chip with no name";
    EXPECT_EQ(statuses[0].metric.provider, "hwmon:hwmon2");
    EXPECT_FALSE(statuses[0].unavailable.has_value()) << "but its temperature is still read";
  }
}

TEST_F(ThermalTest, AnEmptyNameIsTheSameAsNone) {
  fs.set(fixtures::hwmon_dir(5) + "/name", "\n");
  add_temp(fs, 5, 1, "30000");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.provider, "hwmon:hwmon5");
}

TEST_F(ThermalTest, AnUnreadableLabelLeavesTheSensorUnidentifiedNotHidden) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  fs.fail_open(fixtures::hwmon_dir(0) + "/temp1_label", EACCES);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.sensor");
  EXPECT_FALSE(statuses[0].unavailable.has_value());
}

TEST_F(ThermalTest, ZonesAreFoundFromZero) {
  add_zone(fs, 0, "acpitz", "27800");
  add_zone(fs, 63, "acpitz", "27900");
  add_zone(fs, 64, "acpitz", "28000");
  const std::vector<MetricStatus> statuses = source.probe();
  EXPECT_EQ(statuses.size(), 2U) << "a gap in the numbering is not a finding; zone 64 is past the bound";
}

TEST_F(ThermalTest, ProbingAgainStartsFromTheMachineAsItIsNow) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  EXPECT_EQ(source.probe().size(), 1U);
  EXPECT_EQ(sample(source).size(), 1U);
  fs.remove(fixtures::temp_path(0, 1));
  const std::vector<MetricStatus> again = source.probe();
  ASSERT_EQ(again.size(), 1U);
  EXPECT_EQ(again[0].unavailable->kind, Availability::kAbsent);
  EXPECT_TRUE(sample(source).empty());
}

TEST_F(ThermalTest, NothingIsSampledBeforeTheProbeAndSamplingAppends) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "50000");
  EXPECT_TRUE(sample(source).empty());
  static_cast<void>(source.probe());
  std::vector<Reading> readings = {Gauge{-1.0}};
  source.sample(readings);
  source.sample(readings);
  EXPECT_EQ(readings.size(), 3U);
  EXPECT_EQ(source.name(), "thermal");
}

// --- the states of one sensor, at the probe -------------------------------------------------------

TEST_F(ThermalTest, ASensorThatCannotBeReadIsReportedWithItsStateAndARemedyForThatState) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  add_temp(fs, 0, 2, "50000", "Core 0");
  add_temp(fs, 0, 3, "garbage", "Core 1");
  add_temp(fs, 0, 4, "50000", "Core 2");
  fs.make_directory(fixtures::temp_path(0, 5));
  fs.fail_open(fixtures::temp_path(0, 1), EACCES);
  fs.fail_read(fixtures::temp_path(0, 2), EIO);
  fs.fail_open(fixtures::temp_path(0, 4), EPERM);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 5U);

  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_EQ(statuses[0].unavailable->path, fixtures::temp_path(0, 1));
  EXPECT_NE(statuses[0].unavailable->remediation.find("hwmon:coretemp"), std::string::npos);
  EXPECT_NE(statuses[0].unavailable->remediation.find("udev"), std::string::npos);
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kUnreadable);
  EXPECT_NE(statuses[1].unavailable->remediation.find("dmesg"), std::string::npos);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
  EXPECT_NE(statuses[2].unavailable->remediation.find("not a temperature"), std::string::npos);
  EXPECT_EQ(statuses[3].unavailable->kind, Availability::kDenied) << "EPERM too";
  EXPECT_EQ(statuses[4].unavailable->kind, Availability::kUnreadable) << "a directory, EISDIR";
  EXPECT_TRUE(sample(source).empty()) << "none of them is present, so none is read";
}

TEST_F(ThermalTest, EveryMalformedValueIsRefusedWithTheReason) {
  const std::string values[] = {"",        "abc",         "12.5",       "50000 ",  " 50000", "+50000",
                                "0x1388", "4294967295",  "-5-",     "99999999999999999999",
                                "1e3"};
  add_chip(fs, 0, "nct6775");
  int input = 1;
  for (const std::string& value : values) {
    add_temp(fs, 0, input++, value);
  }
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), std::size(values));
  for (std::size_t at = 0; at < statuses.size(); ++at) {
    ASSERT_TRUE(statuses[at].unavailable.has_value()) << "accepted \"" << values[at] << "\"";
    EXPECT_EQ(statuses[at].unavailable->kind, Availability::kMalformed) << values[at];
    EXPECT_FALSE(statuses[at].unavailable->detail.empty()) << values[at];
  }
}

TEST_F(ThermalTest, ASensorThatIsNotThereYetOrNoLongerAtTheProbeIsAbsentAndSkipped) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  add_temp(fs, 0, 2, "50000", "Core 0");
  add_temp(fs, 0, 3, "50000", "Core 1");
  fs.fail_open(fixtures::temp_path(0, 2), ENODEV);  // the driver says the device went
  fs.fail_open(fixtures::temp_path(0, 3), ENXIO);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "thermal.package");
}

TEST_F(ThermalTest, AZoneIsInTheSameStatesASensorIs) {
  add_zone(fs, 0, "acpitz", "27800");
  add_zone(fs, 1, "acpitz", "27800");
  add_zone(fs, 2, "acpitz", "nope");
  fs.fail_open(fixtures::zone_path(0), EACCES);
  fs.fail_read(fixtures::zone_path(1), EIO);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_NE(statuses[0].unavailable->remediation.find("thermal_zone:acpitz"), std::string::npos);
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kUnreadable);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
}

TEST_F(ThermalTest, ASignalStormOnTheReadIsUnreadableNeverAbsent) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "50000");
  fs.fail_read(fixtures::temp_path(0, 1), EINTR);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kUnreadable);
  EXPECT_TRUE(fs.all_closed()) << "no descriptor is left behind by a failed read";
}

// --- the states of one sensor, in the run ----------------------------------------------------------

TEST_F(ThermalTest, ASensorThatDisappearsMidRunIsVanishedAndReadAgainWhenItReturns) {
  add_chip(fs, 0, "coretemp");
  add_temp(fs, 0, 1, "50000", "Package id 0");
  add_temp(fs, 0, 2, "47000", "Core 0");
  static_cast<void>(source.probe());
  EXPECT_EQ(sample(source).size(), 2U);

  fs.remove(fixtures::temp_path(0, 1));
  std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 2U) << "still one reading per metric present at the probe";
  const Unavailable& gone = unavailable_of(readings[0]);
  EXPECT_EQ(gone.kind, Availability::kVanished);
  EXPECT_EQ(gone.path, fixtures::temp_path(0, 1));
  EXPECT_NE(gone.remediation.find("disappeared during the run"), std::string::npos);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 47.0) << "the other sensor is unaffected";

  add_temp(fs, 0, 1, "51000");
  readings = sample(source);
  EXPECT_DOUBLE_EQ(gauge_of(readings[0]), 51.0) << "a metric that comes back is read again";
}

TEST_F(ThermalTest, TheDriverSayingTheDeviceWentIsVanishedToo) {
  add_chip(fs, 0, "nct6775");
  add_temp(fs, 0, 1, "50000");
  static_cast<void>(source.probe());
  fs.fail_read(fixtures::temp_path(0, 1), ENODEV);
  EXPECT_EQ(unavailable_of(sample(source)[0]).kind, Availability::kVanished);
  fs.fail_read(fixtures::temp_path(0, 1), ENXIO);
  EXPECT_EQ(unavailable_of(sample(source)[0]).kind, Availability::kVanished);
}

TEST_F(ThermalTest, ASensorThatTurnsBadMidRunIsReportedAsWhatItNowIsNeverAbsent) {
  add_chip(fs, 0, "nct6775");
  for (int input = 1; input <= 4; ++input) {
    add_temp(fs, 0, input, "50000");
  }
  static_cast<void>(source.probe());
  fs.fail_open(fixtures::temp_path(0, 1), EACCES);
  fs.set(fixtures::temp_path(0, 2), "hot\n");
  fs.fail_read(fixtures::temp_path(0, 3), EIO);
  fs.make_directory(fixtures::temp_path(0, 4));
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 4U);
  EXPECT_EQ(unavailable_of(readings[0]).kind, Availability::kDenied) << "a permission does not vanish";
  EXPECT_EQ(unavailable_of(readings[1]).kind, Availability::kMalformed);
  EXPECT_EQ(unavailable_of(readings[2]).kind, Availability::kUnreadable);
  EXPECT_EQ(unavailable_of(readings[3]).kind, Availability::kUnreadable);
  for (const Reading& reading : readings) {
    EXPECT_FALSE(unavailable_of(reading).remediation.empty());
  }
}

// --- Channel and the remedies -----------------------------------------------------------------------

TEST(ThermalRemedyTest, EachStateHasItsOwnWordsAndPresentHasNone) {
  const Remedies remedies = make_remedies("a", "v", "d", "m", "u");
  EXPECT_EQ(remedies.for_availability(Availability::kAbsent), "a");
  EXPECT_EQ(remedies.for_availability(Availability::kVanished), "v");
  EXPECT_EQ(remedies.for_availability(Availability::kDenied), "d");
  EXPECT_EQ(remedies.for_availability(Availability::kMalformed), "m");
  EXPECT_EQ(remedies.for_availability(Availability::kUnreadable), "u");
  EXPECT_TRUE(remedies.for_availability(Availability::kPresent).empty());
}

TEST(ThermalRemedyTest, AnUnavailableCarriesTheFailureAndTheRemedyForItsKind) {
  const Remedies remedies = make_remedies("a", "v", "d", "m", "u");
  const Unavailable made = make_unavailable(
      topology::Unavailable{Availability::kDenied, "/sys/x", "Permission denied"}, remedies);
  EXPECT_EQ(made, (Unavailable{Availability::kDenied, "/sys/x", "Permission denied", "d"}));
}

// --- the source contract, once per state (ADR 0003) -------------------------------------------------

fixtures::Edit with_coretemp() {
  return [](MemFs& fs) {
    add_chip(fs, 0, "coretemp");
    add_temp(fs, 0, 1, "52000", "Package id 0");
    add_temp(fs, 0, 2, "48500", "Core 0");
    add_zone(fs, 0, "x86_pkg_temp", "51000");
  };
}

struct PresentState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>(with_coretemp());
  }
};

struct AbsentState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>(
        [](MemFs& fs) { fs.load(fixtures::fixture("sysfs/thermal/absent")); });
  }
};

struct DeniedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>([](MemFs& fs) {
      with_coretemp()(fs);
      fs.fail_open(fixtures::temp_path(0, 1), EACCES);
      fs.fail_open(fixtures::zone_path(0), EACCES);
    });
  }
};

struct MalformedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>([](MemFs& fs) {
      with_coretemp()(fs);
      fs.set(fixtures::temp_path(0, 1), "4294967295\n");
      fs.set(fixtures::zone_path(0), "warm\n");
    });
  }
};

struct VanishedState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>(with_coretemp(), [](MemFs& fs) {
      fs.remove(fixtures::temp_path(0, 1));
      fs.remove(fixtures::zone_path(0));
    });
  }
};

struct UnreadableState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<ThermalSource>>([](MemFs& fs) {
      with_coretemp()(fs);
      fs.fail_read(fixtures::temp_path(0, 2), EIO);
    });
  }
};

struct RealMachine {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<fixtures::RealMachineSource<ThermalSource>>();
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

// This file runs the source suite only.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);

namespace {

using ThermalStates = ::testing::Types<telemetry::PresentState, telemetry::AbsentState,
                                       telemetry::DeniedState, telemetry::MalformedState,
                                       telemetry::VanishedState, telemetry::UnreadableState,
                                       telemetry::RealMachine>;

INSTANTIATE_TYPED_TEST_SUITE_P(ThermalSource, SourceContract, ThermalStates);

}  // namespace
}  // namespace loadforge::contracts
