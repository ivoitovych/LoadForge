// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fan source: fan speeds from hwmon.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The six states, as in the thermal tests and through the same substituted
// seam (the fan source reads the same hwmon directories): the captured
// machine (tests/fixtures/sysfs/fan/absent) has no fan input, so the present
// states are built in the layout of the kernel's hwmon sysfs interface
// (thermal/hwmon_tree.hpp -- owned by the thermal tests, which the fan source's
// own library already depends on for hwmon discovery). Fan speeds are gauges;
// they do not wrap.

#include "telemetry/sources/fan/fan.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "contracts/telemetry/source_contract.hpp"
#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"
#include "unit/telemetry/sources/thermal/hwmon_tree.hpp"
#include "unit/telemetry/sources/thermal/owned.hpp"

namespace loadforge::telemetry {
namespace {

using fixtures::add_chip;
using fixtures::add_fan;
using fixtures::fan_path;
using fixtures::MemFs;
using fixtures::OwnedSource;
using topology::Availability;

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

Availability kind_of(const Reading& reading) {
  const auto* unavailable = std::get_if<Unavailable>(&reading);
  EXPECT_NE(unavailable, nullptr);
  return unavailable == nullptr ? Availability::kPresent : unavailable->kind;
}

class FanTest : public ::testing::Test {
 protected:
  MemFs fs;
  platform::FileSystem files{fs};
  FanSource source{files};
};

TEST_F(FanTest, TheCapturedMachineHasNoFanAndSaysSoWithARemedy) {
  fs.load(fixtures::fixture("sysfs/fan/absent"));
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "fan.speed");
  EXPECT_EQ(statuses[0].metric.unit, Unit::kRpm);
  ASSERT_TRUE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[0].unavailable->path, "/sys/class/hwmon");
  EXPECT_NE(statuses[0].unavailable->remediation.find("virtual machines"), std::string::npos);
  EXPECT_NE(statuses[0].unavailable->remediation.find("nct6775"), std::string::npos);
  EXPECT_TRUE(sample(source).empty()) << "an absent fan is not a fan at zero rpm";
  EXPECT_EQ(source.name(), "fan");
}

TEST_F(FanTest, ARootPrefixesEveryPath) {
  FanSource rooted(files, "/mnt/machine");
  fs.set("/mnt/machine/sys/class/hwmon/hwmon1/name", "nct6775\n");
  fs.set("/mnt/machine/sys/class/hwmon/hwmon1/fan2_input", "1200\n");
  const std::vector<MetricStatus> statuses = rooted.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_EQ(FanSource(files, "/mnt/none").probe()[0].unavailable->path,
            "/mnt/none/sys/class/hwmon");
}

TEST_F(FanTest, EveryFanInputOfEveryChipIsADeviceSeriesOfItsOwn) {
  add_chip(fs, 0, "coretemp");
  add_chip(fs, 1, "nct6775");
  add_fan(fs, 1, 1, "1180");
  add_fan(fs, 1, 3, "850");
  add_chip(fs, 4, "it8728");
  add_fan(fs, 4, 1, "2400");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);
  for (const MetricStatus& status : statuses) {
    EXPECT_EQ(status.metric.name, "fan.speed");
    EXPECT_EQ(status.metric.kind, Kind::kGauge);
    EXPECT_EQ(status.metric.scope, Scope::kDevice);
    EXPECT_FALSE(status.unavailable.has_value());
  }
  EXPECT_EQ(statuses[0].metric.provider, "hwmon:nct6775");
  EXPECT_EQ(statuses[0].metric.scope_index, (1U << 8U) | 1U);
  EXPECT_EQ(statuses[1].metric.scope_index, (1U << 8U) | 3U);
  EXPECT_EQ(statuses[2].metric.provider, "hwmon:it8728");
  EXPECT_EQ(statuses[2].metric.scope_index, (4U << 8U) | 1U);

  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 3U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[0]), 1180.0);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 850.0);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), 2400.0);
}

TEST_F(FanTest, AStoppedFanIsAReadingOfZeroAndNotAnAbsence) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 1, "0");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_DOUBLE_EQ(gauge_of(sample(source)[0]), 0.0);
}

TEST_F(FanTest, TheBoundsOfAPlausibleSpeed) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 1, "1000000");
  add_fan(fs, 0, 2, "1000001");
  add_fan(fs, 0, 3, "-1");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 3U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kMalformed);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
}

TEST_F(FanTest, InputsAreFoundUpTo255AndGapsAreNotFindings) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 255, "900");
  add_fan(fs, 0, 256, "900");
  EXPECT_EQ(source.probe().size(), 1U);
}

TEST_F(FanTest, AChipWithNoFanContributesNothingAndAChipWithNoNameIsNotThere) {
  add_chip(fs, 0, "coretemp");
  fs.set(fixtures::hwmon_dir(1) + "/fan1_input", "1000\n");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
}

TEST_F(FanTest, AChipWhoseNameCannotBeReadStillHasItsFansReadUnderANumberedProvider) {
  fs.set(fixtures::hwmon_dir(2) + "/name", "nct6775\n");
  fs.fail_open(fixtures::hwmon_dir(2) + "/name", EACCES);
  add_fan(fs, 2, 1, "1500");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.provider, "hwmon:hwmon2");
  EXPECT_FALSE(statuses[0].unavailable.has_value());
}

TEST_F(FanTest, AFanThatCannotBeReadIsReportedWithItsStateAndARemedyForThatState) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 1, "1000");
  add_fan(fs, 0, 2, "1000");
  add_fan(fs, 0, 3, "fast");
  add_fan(fs, 0, 4, "1000");
  fs.make_directory(fan_path(0, 5));
  fs.fail_open(fan_path(0, 1), EACCES);
  fs.fail_read(fan_path(0, 2), EIO);
  fs.fail_open(fan_path(0, 4), ENODEV);  // the device behind it has gone: nothing to read
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 4U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_NE(statuses[0].unavailable->remediation.find("hwmon:nct6775"), std::string::npos);
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kUnreadable);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
  EXPECT_NE(statuses[2].unavailable->remediation.find("not a fan speed"), std::string::npos);
  EXPECT_EQ(statuses[3].unavailable->kind, Availability::kUnreadable) << "a directory, EISDIR";
  EXPECT_TRUE(sample(source).empty());
}

TEST_F(FanTest, AFanThatDisappearsMidRunIsVanishedAndReadAgainWhenItReturns) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 1, "1000");
  add_fan(fs, 0, 2, "2000");
  static_cast<void>(source.probe());
  fs.remove(fan_path(0, 1));
  std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 2U);
  EXPECT_EQ(kind_of(readings[0]), Availability::kVanished);
  EXPECT_NE(std::get<Unavailable>(readings[0]).remediation.find("disappeared during the run"),
            std::string::npos);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 2000.0);
  add_fan(fs, 0, 1, "1100");
  EXPECT_DOUBLE_EQ(gauge_of(sample(source)[0]), 1100.0);
}

TEST_F(FanTest, AFanThatTurnsBadMidRunIsReportedAsWhatItNowIsNeverAbsent) {
  add_chip(fs, 0, "nct6775");
  for (int input = 1; input <= 3; ++input) {
    add_fan(fs, 0, input, "1000");
  }
  static_cast<void>(source.probe());
  fs.fail_open(fan_path(0, 1), EACCES);
  fs.set(fan_path(0, 2), "stuck\n");
  fs.fail_read(fan_path(0, 3), EIO);
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 3U);
  EXPECT_EQ(kind_of(readings[0]), Availability::kDenied);
  EXPECT_EQ(kind_of(readings[1]), Availability::kMalformed);
  EXPECT_EQ(kind_of(readings[2]), Availability::kUnreadable);
}

TEST_F(FanTest, NothingIsSampledBeforeTheProbeAndProbingAgainStartsOver) {
  add_chip(fs, 0, "nct6775");
  add_fan(fs, 0, 1, "1000");
  EXPECT_TRUE(sample(source).empty());
  EXPECT_EQ(source.probe().size(), 1U);
  EXPECT_EQ(sample(source).size(), 1U);
  fs.remove(fan_path(0, 1));
  EXPECT_EQ(source.probe()[0].unavailable->kind, Availability::kAbsent);
  EXPECT_TRUE(sample(source).empty());
}

// --- the source contract, once per state (ADR 0003)

fixtures::Edit with_fans() {
  return [](MemFs& fs) {
    add_chip(fs, 0, "coretemp");
    add_chip(fs, 1, "nct6775");
    add_fan(fs, 1, 1, "1180");
    add_fan(fs, 1, 2, "0");
  };
}

struct PresentState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FanSource>>(with_fans());
  }
};

struct AbsentState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FanSource>>(
        [](MemFs& fs) { fs.load(fixtures::fixture("sysfs/fan/absent")); });
  }
};

struct DeniedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FanSource>>([](MemFs& fs) {
      with_fans()(fs);
      fs.fail_open(fan_path(1, 1), EACCES);
    });
  }
};

struct MalformedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FanSource>>([](MemFs& fs) {
      with_fans()(fs);
      fs.set(fan_path(1, 1), "-1\n");
    });
  }
};

struct VanishedState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FanSource>>(with_fans(),
                                                    [](MemFs& fs) { fs.remove(fan_path(1, 1)); });
  }
};

struct RealMachine {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<fixtures::RealMachineSource<FanSource>>();
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

namespace {

using FanStates =
    ::testing::Types<telemetry::PresentState, telemetry::AbsentState, telemetry::DeniedState,
                     telemetry::MalformedState, telemetry::VanishedState, telemetry::RealMachine>;

INSTANTIATE_TYPED_TEST_SUITE_P(FanSource, SourceContract, FanStates);

}  // namespace
}  // namespace loadforge::contracts
