// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trivial reference telemetry sources and timeline, run against their
// suites, and the definitions src/telemetry/sample.hpp and source.hpp carry.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/telemetry/source_contract.hpp"
#include "contracts/telemetry/timeline_contract.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::contracts {
namespace {

using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Kind;
using telemetry::Metric;
using telemetry::MetricStatus;
using telemetry::Reading;
using telemetry::Scope;
using telemetry::Unavailable;
using telemetry::Unit;
using topology::Availability;

/// A thermal source with a sensor that vanishes and comes back, a counter
/// that wraps, and a sensor this machine never had.
class ScriptedThermal final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "thermal"; }
  std::vector<MetricStatus> probe() override {
    probed_ = true;
    return {
        {Metric{"thermal.package", Unit::kCelsius, Kind::kGauge, Scope::kPackage, 0, {}, "hwmon"},
         std::nullopt},
        {Metric{"thermal.vrm", Unit::kCelsius, Kind::kGauge, Scope::kPackage, 0, {}, "hwmon"},
         Unavailable{Availability::kAbsent, "/sys/class/hwmon", "no VRM sensor is exposed",
                     "this board's drivers do not publish one; nothing to change"}},
        {Metric{"thermal.trips", Unit::kCount, Kind::kCounter, Scope::kMachine, 0, 1000,
                "thermal_zone"},
         std::nullopt},
    };
  }
  void sample(std::vector<Reading>& out) override {
    if (!probed_) {
      return;
    }
    if (round_ % 2 == 1) {
      out.emplace_back(Unavailable{Availability::kVanished, "/sys/class/hwmon/hwmon0/temp1_input",
                                   "gone since it was last read", "check the sensor driver"});
    } else {
      out.emplace_back(Gauge{40.0 + static_cast<double>(round_)});
    }
    out.emplace_back(Counter{(990 + 7 * round_) % 1000});
    ++round_;
  }

 private:
  bool probed_ = false;
  std::uint64_t round_ = 0;
};

/// A fan source on a machine with no fan sensor at all.
class EmptyFan final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fan"; }
  std::vector<MetricStatus> probe() override {
    return {{Metric{"fan.speed", Unit::kRpm, Kind::kGauge, Scope::kMachine, 0, {}, "hwmon"},
             Unavailable{Availability::kAbsent, "/sys/class/hwmon", "no fan input is exposed",
                         "this platform does not publish fan speed; nothing to change"}}};
  }
  void sample(std::vector<Reading>& /*out*/) override {}
};

struct ReferenceThermal {
  static constexpr int kRounds = 4;
  static std::unique_ptr<telemetry::Source> make() { return std::make_unique<ScriptedThermal>(); }
};

struct ReferenceEmptyFan {
  static constexpr int kRounds = 1;
  static std::unique_ptr<telemetry::Source> make() { return std::make_unique<EmptyFan>(); }
};

using ReferenceSources = ::testing::Types<ReferenceThermal, ReferenceEmptyFan>;
INSTANTIATE_TYPED_TEST_SUITE_P(Reference, SourceContract, ReferenceSources);

class VectorTimeline final : public telemetry::Timeline {
 public:
  explicit VectorTimeline(std::size_t capacity) : capacity_(capacity) {}

  core::Result<telemetry::SeriesId, telemetry::TimelineError> add_series(
      const Metric& metric) override {
    for (const Metric& existing : metrics_) {
      if (telemetry::same_series(existing, metric)) {
        return telemetry::TimelineError{telemetry::TimelineFault::kDuplicateSeries, metric.name};
      }
    }
    metrics_.push_back(metric);
    points_.emplace_back();
    return static_cast<telemetry::SeriesId>(metrics_.size() - 1);
  }

  core::Result<core::Ok, telemetry::TimelineError> append(telemetry::SeriesId series,
                                                          const telemetry::Point& point) override {
    using telemetry::TimelineError;
    using telemetry::TimelineFault;
    if (series >= metrics_.size()) {
      return TimelineError{TimelineFault::kUnknownSeries, std::to_string(series)};
    }
    if (!points_[series].empty() && point.timestamp_ns <= points_[series].back().timestamp_ns) {
      return TimelineError{TimelineFault::kOutOfOrder, std::to_string(point.timestamp_ns)};
    }
    const Kind kind = metrics_[series].kind;
    if ((kind == Kind::kGauge && std::holds_alternative<Counter>(point.reading)) ||
        (kind == Kind::kCounter && std::holds_alternative<Gauge>(point.reading))) {
      return TimelineError{TimelineFault::kKindMismatch, metrics_[series].name};
    }
    if (total_ == capacity_) {
      return TimelineError{TimelineFault::kCapacity, std::to_string(capacity_)};
    }
    points_[series].push_back(point);
    ++total_;
    return core::Ok{};
  }

  core::Result<core::Ok, telemetry::TimelineError> mark_phase(telemetry::PhaseMark mark) override {
    if (!phases_.empty() && mark.start_ns <= phases_.back().start_ns) {
      return telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, mark.label};
    }
    phases_.push_back(std::move(mark));
    return core::Ok{};
  }

  [[nodiscard]] std::size_t series_count() const noexcept override { return metrics_.size(); }
  [[nodiscard]] const Metric& metric(telemetry::SeriesId series) const override {
    return metrics_.at(series);
  }
  [[nodiscard]] std::span<const telemetry::Point> points(
      telemetry::SeriesId series) const override {
    return points_.at(series);
  }
  [[nodiscard]] std::span<const telemetry::PhaseMark> phases() const override { return phases_; }

 private:
  std::size_t capacity_;
  std::size_t total_ = 0;
  std::vector<Metric> metrics_;
  std::vector<std::vector<telemetry::Point>> points_;
  std::vector<telemetry::PhaseMark> phases_;
};

struct ReferenceTimeline {
  static std::unique_ptr<telemetry::Timeline> make(std::size_t capacity) {
    return std::make_unique<VectorTimeline>(capacity);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, TimelineContract, ::testing::Types<ReferenceTimeline>);

TEST(TelemetryDefinition, UnitAndScopeNames) {
  using telemetry::describe;
  EXPECT_EQ(describe(Unit::kCelsius), "celsius");
  EXPECT_EQ(describe(Unit::kWatts), "watts");
  EXPECT_EQ(describe(Unit::kJoules), "joules");
  EXPECT_EQ(describe(Unit::kHertz), "hertz");
  EXPECT_EQ(describe(Unit::kRpm), "rpm");
  EXPECT_EQ(describe(Unit::kBytes), "bytes");
  EXPECT_EQ(describe(Unit::kRatio), "ratio");
  EXPECT_EQ(describe(Unit::kCount), "count");
  EXPECT_EQ(describe(Unit::kVolts), "volts");
  EXPECT_EQ(describe(Unit::kMicrojoules), "microjoules");
  EXPECT_EQ(describe(static_cast<Unit>(99)), "unrecognised unit");
  EXPECT_EQ(describe(Scope::kMachine), "machine");
  EXPECT_EQ(describe(Scope::kPackage), "package");
  EXPECT_EQ(describe(Scope::kCore), "core");
  EXPECT_EQ(describe(Scope::kCpu), "cpu");
  EXPECT_EQ(describe(Scope::kNumaNode), "node");
  EXPECT_EQ(describe(Scope::kMemoryController), "memory controller");
  EXPECT_EQ(describe(Scope::kDimm), "dimm");
  EXPECT_EQ(describe(Scope::kDevice), "device");
  EXPECT_EQ(describe(static_cast<Scope>(99)), "unrecognised scope");
  EXPECT_EQ(describe(telemetry::TimelineFault::kDuplicateSeries), "duplicate series");
  EXPECT_EQ(describe(telemetry::TimelineFault::kUnknownSeries), "unknown series");
  EXPECT_EQ(describe(telemetry::TimelineFault::kOutOfOrder), "out of order");
  EXPECT_EQ(describe(telemetry::TimelineFault::kKindMismatch), "reading of the wrong kind");
  EXPECT_EQ(describe(telemetry::TimelineFault::kCapacity), "capacity reached");
  EXPECT_EQ(describe(static_cast<telemetry::TimelineFault>(99)), "unrecognised timeline fault");
}

TEST(TelemetryDefinition, MetricNamesAreTwoOrMoreDottedLowerCaseParts) {
  using telemetry::is_valid_metric_name;
  EXPECT_TRUE(is_valid_metric_name("thermal.package"));
  EXPECT_TRUE(is_valid_metric_name("edac.mc0.corrected_count"));
  EXPECT_TRUE(is_valid_metric_name("a.b2"));
  EXPECT_FALSE(is_valid_metric_name(""));
  EXPECT_FALSE(is_valid_metric_name("thermal")) << "one part";
  EXPECT_FALSE(is_valid_metric_name("thermal.")) << "an empty last part";
  EXPECT_FALSE(is_valid_metric_name("thermal.package.")) << "and after two good ones";
  EXPECT_FALSE(is_valid_metric_name(".package")) << "an empty first part";
  EXPECT_FALSE(is_valid_metric_name("thermal..package")) << "an empty middle part";
  EXPECT_FALSE(is_valid_metric_name("thermal.2nd")) << "a part starting with a digit";
  EXPECT_FALSE(is_valid_metric_name("Thermal.package"));
  EXPECT_FALSE(is_valid_metric_name("thermal.pack-age"));
  EXPECT_FALSE(is_valid_metric_name("thermal.pa{"));
  EXPECT_FALSE(is_valid_metric_name("thermal.a:"));
}

TEST(TelemetryDefinition, ASeriesIsItsNameScopeAndIndex) {
  const Metric a{"power.package", Unit::kWatts, Kind::kGauge, Scope::kPackage, 0, {}, "rapl"};
  Metric b = a;
  b.provider = "perf";
  b.unit = Unit::kJoules;
  EXPECT_TRUE(telemetry::same_series(a, b)) << "the provider does not make another series";
  b.scope_index = 1;
  EXPECT_FALSE(telemetry::same_series(a, b));
}

TEST(TelemetryDefinition, ACounterAdvancesAcrossAtMostOneWrap) {
  using telemetry::counter_delta;
  EXPECT_EQ(counter_delta(10, 25, std::nullopt), 15U);
  EXPECT_EQ(counter_delta(10, 10, 1000U), 0U);
  EXPECT_EQ(counter_delta(990, 5, 1000U), 15U) << "wrapped at the modulus";
  EXPECT_EQ(counter_delta(990, 995, 1000U), 5U);
  EXPECT_EQ(counter_delta(~std::uint64_t{0}, 4, std::nullopt), 5U) << "wrapped at 2^64";
}

TEST(TelemetryDefinition, ACounterWithNoModulusDoesNotWrapAndAStepDownIsAReset) {
  using telemetry::counter_advance;
  EXPECT_EQ(counter_advance(10, 25, std::nullopt), std::optional<std::uint64_t>{15});
  EXPECT_EQ(counter_advance(10, 10, std::nullopt), std::optional<std::uint64_t>{0});
  EXPECT_EQ(counter_advance(5, 0, std::nullopt), std::nullopt) << "reset, not 2^64 - 5";
  EXPECT_EQ(counter_advance(~std::uint64_t{0}, 4, std::nullopt), std::nullopt)
      << "a 64-bit counter does not wrap within a run";
  EXPECT_EQ(counter_advance(990, 5, 1000U), std::optional<std::uint64_t>{15}) << "wrapped";
  EXPECT_EQ(counter_advance(990, 995, 1000U), std::optional<std::uint64_t>{5});
  EXPECT_EQ(counter_advance(10, 10, 1000U), std::optional<std::uint64_t>{0});
}

TEST(TelemetryDefinition, TheReportCountsMetricsAndPresentOnes) {
  ScriptedThermal thermal;
  EmptyFan fan;
  const telemetry::CapabilityReport report{{{"thermal", thermal.probe()}, {"fan", fan.probe()}}};
  const telemetry::CapabilityCounts counts = telemetry::count(report);
  EXPECT_EQ(counts.metrics, 4U);
  EXPECT_EQ(counts.present, 2U);
  EXPECT_EQ(telemetry::count(telemetry::CapabilityReport{}).metrics, 0U);
}

TEST(TelemetryDefinition, ReadingsAreTheSameAlternativeAndEqualWithinIt) {
  using telemetry::same_reading;
  const Unavailable gone{Availability::kVanished, "p", "d", "r"};
  EXPECT_TRUE(same_reading(Gauge{1.0}, Gauge{1.0}));
  EXPECT_FALSE(same_reading(Gauge{1.0}, Gauge{2.0}));
  EXPECT_TRUE(same_reading(Counter{3}, Counter{3}));
  EXPECT_FALSE(same_reading(Counter{3}, Counter{4}));
  EXPECT_TRUE(same_reading(gone, gone));
  EXPECT_FALSE(same_reading(gone, Unavailable{}));
  EXPECT_FALSE(same_reading(Gauge{3.0}, Counter{3})) << "a gauge of 3 is not a count of 3";
}

TEST(TelemetryEquality, EveryMemberIsCompared) {
  const Metric metric{"power.package", Unit::kWatts, Kind::kCounter, Scope::kPackage, 0, 10,
                      "rapl"};
  expect_memberwise_equality(
      metric, {[](Metric& m) { m.name = "power.dram"; }, [](Metric& m) { m.unit = Unit::kJoules; },
               [](Metric& m) { m.kind = Kind::kGauge; }, [](Metric& m) { m.scope = Scope::kCpu; },
               [](Metric& m) { m.scope_index = 1; }, [](Metric& m) { m.modulus.reset(); },
               [](Metric& m) { m.provider = "perf"; }});
  const Unavailable unavailable{Availability::kDenied, "p", "d", "r"};
  expect_memberwise_equality(
      unavailable,
      {[](Unavailable& u) { u.kind = Availability::kAbsent; }, [](Unavailable& u) { u.path = "q"; },
       [](Unavailable& u) { u.detail = "e"; }, [](Unavailable& u) { u.remediation = "s"; }});
  expect_memberwise_equality(Gauge{1.0}, {[](Gauge& g) { g.value = 2.0; }});
  expect_memberwise_equality(Counter{1}, {[](Counter& c) { c.raw = 2; }});
  expect_memberwise_equality(MetricStatus{metric, std::nullopt},
                             {[](MetricStatus& s) { s.metric.scope_index = 3; },
                              [](MetricStatus& s) { s.unavailable = Unavailable{}; }});
  expect_memberwise_equality(telemetry::SourceStatus{"power", {{metric, std::nullopt}}},
                             {[](telemetry::SourceStatus& s) { s.source = "thermal"; },
                              [](telemetry::SourceStatus& s) { s.metrics.clear(); }});
  expect_memberwise_equality(
      telemetry::CapabilityReport{{{"power", {}}}},
      {[](telemetry::CapabilityReport& r) { r.sources.push_back({"fan", {}}); }});
  expect_memberwise_equality(telemetry::Point{1, Gauge{1.0}},
                             {[](telemetry::Point& p) { p.timestamp_ns = 2; },
                              [](telemetry::Point& p) { p.reading = Counter{1}; }});
  expect_memberwise_equality(telemetry::PhaseMark{1, "a"},
                             {[](telemetry::PhaseMark& p) { p.start_ns = 2; },
                              [](telemetry::PhaseMark& p) { p.label = "b"; }});
  expect_memberwise_equality(
      telemetry::TimelineError{telemetry::TimelineFault::kCapacity, "d"},
      {[](telemetry::TimelineError& e) { e.fault = telemetry::TimelineFault::kOutOfOrder; },
       [](telemetry::TimelineError& e) { e.detail = "e"; }});
}

}  // namespace
}  // namespace loadforge::contracts
