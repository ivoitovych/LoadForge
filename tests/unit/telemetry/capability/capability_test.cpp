// SPDX-License-Identifier: GPL-3.0-or-later
//
// The capability probe: what a machine offers, probed once at the start of a
// run, and rendered with the remedy for every metric that is not there.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The probe has no capability state of its own; it carries whatever the
// sources it is given report, so the six states reach it as the statuses of
// scripted sources (this module's own mock of the Source contract, run
// against the source suite, ADR 0003) and, for the real-against-real tier, from
// the four real sources over the machine captured in tests/fixtures
// (thermal/fan/frequency absent, procfs present) and over the machine the tests
// run on.

#include "telemetry/capability/capability.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contracts/telemetry/telemetry_contract.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/fan/fan.hpp"
#include "telemetry/sources/frequency/frequency.hpp"
#include "telemetry/sources/memory/memory.hpp"
#include "telemetry/sources/thermal/thermal.hpp"
#include "topology/source.hpp"
#include "unit/telemetry/sources/thermal/memfs.hpp"
#include "unit/telemetry/sources/thermal/owned.hpp"

namespace loadforge::telemetry {
namespace {

using topology::Availability;

MetricStatus present(std::string name, Scope scope = Scope::kMachine, std::uint32_t index = 0) {
  return MetricStatus{
      Metric{std::move(name), Unit::kCelsius, Kind::kGauge, scope, index, std::nullopt, "test"},
      std::nullopt};
}

MetricStatus missing(std::string name, Availability kind, std::string remedy,
                     Scope scope = Scope::kMachine, std::uint32_t index = 0) {
  return MetricStatus{
      Metric{std::move(name), Unit::kCelsius, Kind::kGauge, scope, index, std::nullopt, "test"},
      Unavailable{kind, "/sys/where/" + std::to_string(index), "why " + std::to_string(index),
                  std::move(remedy)}};
}

/// This module's own reading of the Source contract: a source that says what it
/// is told to and counts how it is used.
class ScriptedSource final : public Source {
 public:
  ScriptedSource(std::string name, std::vector<MetricStatus> statuses)
      : name_(std::move(name)), statuses_(std::move(statuses)) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] std::vector<MetricStatus> probe() override {
    ++probes_;
    probed_ = true;
    return statuses_;
  }
  void sample(std::vector<Reading>& out) override {
    if (!probed_) {
      return;
    }
    for (const MetricStatus& status : statuses_) {
      if (!status.unavailable.has_value()) {
        out.emplace_back(Gauge{1.0});
      }
    }
  }

  [[nodiscard]] int probes() const noexcept { return probes_; }

 private:
  std::string name_;
  std::vector<MetricStatus> statuses_;
  bool probed_ = false;
  int probes_ = 0;
};

// --- probing

TEST(CapabilityProbeTest, EverySourceIsProbedOnceAndInTheOrderGiven) {
  ScriptedSource thermal("thermal", {present("thermal.package")});
  ScriptedSource fan("fan", {missing("fan.speed", Availability::kAbsent, "none")});
  ScriptedSource memory("memory", {present("memory.total"), present("memory.free")});
  Source* const sources[] = {&thermal, &fan, &memory};

  const CapabilityReport report = probe_capabilities(sources);
  ASSERT_EQ(report.sources.size(), 3U);
  EXPECT_EQ(report.sources[0].source, "thermal");
  EXPECT_EQ(report.sources[1].source, "fan");
  EXPECT_EQ(report.sources[2].source, "memory");
  EXPECT_EQ(report.sources[2].metrics.size(), 2U);
  EXPECT_EQ(report.sources[1].metrics[0].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(thermal.probes(), 1);
  EXPECT_EQ(fan.probes(), 1);
  EXPECT_EQ(memory.probes(), 1);
  EXPECT_EQ(count(report).metrics, 4U);
  EXPECT_EQ(count(report).present, 3U);
}

TEST(CapabilityProbeTest, ASourceProbedHereIsReadyToBeSampledByTheSamplerThatFollows) {
  ScriptedSource thermal("thermal", {present("thermal.package")});
  Source* const sources[] = {&thermal};
  static_cast<void>(probe_capabilities(sources));
  std::vector<Reading> readings;
  thermal.sample(readings);
  EXPECT_EQ(readings.size(), 1U);
  EXPECT_EQ(thermal.probes(), 1) << "the report did not need a second probe";
}

TEST(CapabilityProbeTest, NoSourcesIsAnEmptyReportThatSaysSo) {
  const CapabilityReport report = probe_capabilities({});
  EXPECT_TRUE(report.sources.empty());
  EXPECT_EQ(render_capabilities(report), "telemetry capability: 0 of 0 metric(s) present\n");
}

// --- rendering

TEST(CapabilityRenderTest, AReportReadsAsOneBlockPerSourceWithTheStateWhereWhyAndRemedy) {
  CapabilityReport report;
  report.sources.push_back(SourceStatus{"thermal",
                                        {present("thermal.package", Scope::kPackage, 0),
                                         missing("thermal.sensor", Availability::kDenied,
                                                 "check the udev rule", Scope::kDevice, 7)}});
  report.sources.push_back(
      SourceStatus{"fan", {missing("fan.speed", Availability::kAbsent, "no fan is exposed here")}});

  EXPECT_EQ(render_capabilities(report),
            "telemetry capability: 1 of 3 metric(s) present\n"
            "  thermal: 1 of 2 present\n"
            "    present: thermal.package\n"
            "    thermal.sensor (device 7): not permitted\n"
            "      where:  /sys/where/7\n"
            "      why:    why 7\n"
            "      remedy: check the udev rule\n"
            "  fan: 0 of 1 present\n"
            "    fan.speed (machine 0): not offered by this kernel\n"
            "      where:  /sys/where/0\n"
            "      why:    why 0\n"
            "      remedy: no fan is exposed here\n");
}

TEST(CapabilityRenderTest, TheSameSensorOnManyCpusIsOneEntryWithACount) {
  CapabilityReport report;
  SourceStatus frequency{"frequency", {}};
  for (std::uint32_t cpu = 0; cpu < 256; ++cpu) {
    frequency.metrics.push_back(missing("frequency.cpu", Availability::kAbsent,
                                        "a virtual machine has none", Scope::kCpu, cpu));
  }
  frequency.metrics.push_back(present("frequency.busy_ticks"));
  frequency.metrics.push_back(present("frequency.total_ticks"));
  report.sources.push_back(std::move(frequency));

  const std::string text = render_capabilities(report);
  EXPECT_EQ(text,
            "telemetry capability: 2 of 258 metric(s) present\n"
            "  frequency: 2 of 258 present\n"
            "    present: frequency.busy_ticks frequency.total_ticks\n"
            "    frequency.cpu x256: not offered by this kernel\n"
            "      where:  /sys/where/0\n"
            "      why:    why 0\n"
            "      remedy: a virtual machine has none\n")
      << "256 CPUs print one entry, not 256";
}

TEST(CapabilityRenderTest, PresentMetricsOfOneNameAreCountedAndAnotherNameIsListedAfter) {
  CapabilityReport report;
  report.sources.push_back(SourceStatus{
      "thermal",
      {present("thermal.core", Scope::kCore, 0), present("thermal.package"),
       present("thermal.core", Scope::kCore, 1), present("thermal.core", Scope::kCore, 2)}});
  EXPECT_NE(render_capabilities(report).find("    present: thermal.core x3 thermal.package\n"),
            std::string::npos);
}

TEST(CapabilityRenderTest, AbsencesAreGroupedOnlyWhenNameStateAndRemedyAreAllTheSame) {
  CapabilityReport report;
  report.sources.push_back(SourceStatus{
      "thermal",
      {missing("thermal.zone", Availability::kDenied, "fix the mode", Scope::kDevice, 0),
       missing("thermal.zone", Availability::kDenied, "fix the mode", Scope::kDevice, 1),
       missing("thermal.zone", Availability::kDenied, "fix the owner", Scope::kDevice, 2),
       missing("thermal.zone", Availability::kVanished, "fix the mode", Scope::kDevice, 3),
       missing("thermal.sensor", Availability::kDenied, "fix the mode", Scope::kDevice, 4)}});
  const std::string text = render_capabilities(report);
  EXPECT_NE(text.find("    thermal.zone x2: not permitted\n      where:  /sys/where/0\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("    thermal.zone (device 2): not permitted\n"), std::string::npos) << text;
  EXPECT_NE(text.find("    thermal.zone (device 3): gone since it was last read\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("    thermal.sensor (device 4): not permitted\n"), std::string::npos) << text;
  EXPECT_NE(text.find("      remedy: fix the owner\n"), std::string::npos)
      << "its own remedy, not the first's";
  EXPECT_EQ(text.find("x3"), std::string::npos);
  EXPECT_EQ(text.find("x5"), std::string::npos);
}

TEST(CapabilityRenderTest, EveryStateIsNamedInWords) {
  CapabilityReport report;
  report.sources.push_back(SourceStatus{
      "x",
      {missing("x.a", Availability::kAbsent, "r1"), missing("x.b", Availability::kVanished, "r2"),
       missing("x.c", Availability::kDenied, "r3"), missing("x.d", Availability::kMalformed, "r4"),
       missing("x.e", Availability::kUnreadable, "r5")}});
  const std::string text = render_capabilities(report);
  for (const char* const word : {"not offered by this kernel", "gone since it was last read",
                                 "not permitted", "not in the expected format", "unreadable"}) {
    EXPECT_NE(text.find(word), std::string::npos) << word;
  }
  for (const char* const remedy : {"r1", "r2", "r3", "r4", "r5"}) {
    EXPECT_NE(text.find(std::string("remedy: ") + remedy + "\n"), std::string::npos) << remedy;
  }
}

// --- real sources, real-against-real

TEST(CapabilityRealSourcesTest, TheCapturedMachineReportsWhatItHasAndSaysWhatItLacks) {
  fixtures::MemFs fs;
  fs.load(fixtures::fixture("sysfs/frequency/absent"));
  fs.load(fixtures::fixture("procfs/meminfo/present"));
  platform::FileSystem files(fs);
  ThermalSource thermal(files);
  FanSource fan(files);
  FrequencySource frequency(files);
  MemorySource memory(files);
  Source* const sources[] = {&thermal, &fan, &frequency, &memory};

  const CapabilityReport report = probe_capabilities(sources);
  ASSERT_EQ(report.sources.size(), 4U);
  EXPECT_EQ(count(report).metrics, 1U + 1U + 6U + 7U);
  EXPECT_EQ(count(report).present, 2U + 7U) << "the two tick counters and the seven memory figures";

  const std::string text = render_capabilities(report);
  EXPECT_NE(text.find("telemetry capability: 9 of 15 metric(s) present\n"), std::string::npos)
      << text;
  EXPECT_NE(text.find("  thermal: 0 of 1 present\n    thermal.package (package 0): not offered by "
                      "this kernel\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("      remedy: this machine exposes no temperature sensor to read"),
            std::string::npos);
  EXPECT_NE(text.find("  fan: 0 of 1 present\n"), std::string::npos);
  EXPECT_NE(text.find("    frequency.cpu x4: not offered by this kernel\n"), std::string::npos)
      << text;
  EXPECT_NE(text.find("    present: frequency.busy_ticks frequency.total_ticks\n"),
            std::string::npos);
  EXPECT_NE(text.find("  memory: 7 of 7 present\n"), std::string::npos) << text;
}

TEST(CapabilityRealSourcesTest, ThisMachineRendersWithARemedyForEveryAbsence) {
  platform::RealSyscalls real;
  platform::FileSystem files(real);
  ThermalSource thermal(files);
  FanSource fan(files);
  FrequencySource frequency(files);
  MemorySource memory(files);
  Source* const sources[] = {&thermal, &fan, &frequency, &memory};

  const CapabilityReport report = probe_capabilities(sources);
  ASSERT_EQ(report.sources.size(), 4U);
  for (const SourceStatus& source : report.sources) {
    ASSERT_FALSE(source.metrics.empty()) << source.source << ": a silent column";
    for (const MetricStatus& status : source.metrics) {
      if (status.unavailable.has_value()) {
        EXPECT_FALSE(status.unavailable->remediation.empty()) << status.metric.name;
      }
    }
  }
  const std::string text = render_capabilities(report);
  EXPECT_EQ(text.rfind("telemetry capability: ", 0), 0U);
  EXPECT_NE(text.find("  memory: "), std::string::npos);
  // MemAvailable has been in every kernel since 3.14, and this one reads it.
  const CapabilityCounts counts = count(CapabilityReport{{report.sources[3]}});
  EXPECT_GE(counts.present, 1U);
}

// --- this module's mock, against the source suite (ADR 0003)

struct MockPresent {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<ScriptedSource>(
        "thermal", std::vector<MetricStatus>{present("thermal.package", Scope::kPackage, 0),
                                             present("thermal.core", Scope::kCore, 0)});
  }
};

struct MockAbsent {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<ScriptedSource>(
        "fan", std::vector<MetricStatus>{missing("fan.speed", Availability::kAbsent, "none here")});
  }
};

struct MockMixed {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<ScriptedSource>(
        "thermal",
        std::vector<MetricStatus>{
            present("thermal.package", Scope::kPackage, 0),
            missing("thermal.sensor", Availability::kDenied, "udev", Scope::kDevice, 1),
            missing("thermal.sensor", Availability::kMalformed, "driver", Scope::kDevice, 2)});
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

// This file runs the source suite only.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);

namespace {

using CapabilityMocks =
    ::testing::Types<telemetry::MockPresent, telemetry::MockAbsent, telemetry::MockMixed>;

INSTANTIATE_TYPED_TEST_SUITE_P(CapabilityScriptedSource, SourceContract, CapabilityMocks);

}  // namespace
}  // namespace loadforge::contracts
