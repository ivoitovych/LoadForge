// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_TELEMETRY_SAMPLER_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_TELEMETRY_SAMPLER_CONTRACT_HPP

// Provider-side suite for src/telemetry/sampler.hpp (ADR 0003, 0004).
//
// SamplerContract<Subject>:
//   Subject::make(std::vector<std::unique_ptr<Source>>, TimelineSink&)
//     -> std::unique_ptr<Sampler>
//
// The suite brings scripted sources -- one of which breaks its own contract
// on demand -- and a timeline that records what it was given.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "telemetry/sampler.hpp"

namespace loadforge::contracts {

namespace sampler_suite {

using telemetry::Metric;
using telemetry::MetricStatus;
using telemetry::Reading;

/// A source of `present` gauge metrics and one absent one, reading `value`
/// plus the round number, appending `extra` readings more than it should.
class Scripted final : public telemetry::Source {
 public:
  Scripted(std::string name, std::size_t present, int extra = 0)
      : name_(std::move(name)), present_(present), extra_(extra) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  std::vector<MetricStatus> probe() override {
    ++probes;
    std::vector<MetricStatus> statuses;
    for (std::size_t at = 0; at < present_; ++at) {
      statuses.push_back({metric(at), std::nullopt});
    }
    statuses.push_back({metric(99), telemetry::Unavailable{topology::Availability::kAbsent, "/x",
                                                           "not offered", "nothing to do"}});
    return statuses;
  }
  void sample(std::vector<Reading>& out) override {
    ++samples;
    const auto count = static_cast<std::ptrdiff_t>(present_) + extra_;
    for (std::ptrdiff_t at = 0; at < count; ++at) {
      out.emplace_back(telemetry::Gauge{static_cast<double>(samples * 10 + at)});
    }
  }
  [[nodiscard]] Metric metric(std::size_t at) const {
    return Metric{name_ + ".m" + std::to_string(at),
                  telemetry::Unit::kCelsius,
                  telemetry::Kind::kGauge,
                  telemetry::Scope::kMachine,
                  0,
                  std::nullopt,
                  "test"};
  }

  int probes = 0;
  int samples = 0;

 private:
  std::string name_;
  std::size_t present_;
  int extra_;
};

class Recording final : public telemetry::TimelineSink {
 public:
  core::Result<telemetry::SeriesId, telemetry::TimelineError> add_series(
      const Metric& metric) override {
    if (refuse_series) {
      return telemetry::TimelineError{telemetry::TimelineFault::kCapacity, "full"};
    }
    series.push_back(metric);
    points.emplace_back();
    return static_cast<telemetry::SeriesId>(series.size() - 1);
  }
  core::Result<core::Ok, telemetry::TimelineError> append(telemetry::SeriesId id,
                                                          const telemetry::Point& point) override {
    if (!points.at(id).empty() && point.timestamp_ns <= points.at(id).back().timestamp_ns) {
      return telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, "stale"};
    }
    points.at(id).push_back(point);
    return core::Ok{};
  }
  core::Result<core::Ok, telemetry::TimelineError> mark_phase(telemetry::PhaseMark) override {
    return core::Ok{};
  }
  bool refuse_series = false;
  std::vector<Metric> series;
  std::vector<std::vector<telemetry::Point>> points;
};

}  // namespace sampler_suite

template <typename Subject>
class SamplerContract : public ::testing::Test {
 protected:
  void build(std::vector<int> extras = {0, 0}) {
    std::vector<std::unique_ptr<telemetry::Source>> sources;
    auto thermal = std::make_unique<sampler_suite::Scripted>("thermal", 2, extras.at(0));
    auto power = std::make_unique<sampler_suite::Scripted>("power", 1, extras.at(1));
    thermal_ = thermal.get();
    power_ = power.get();
    sources.push_back(std::move(thermal));
    sources.push_back(std::move(power));
    sampler_ = Subject::make(std::move(sources), timeline_);
  }
  sampler_suite::Recording timeline_;
  sampler_suite::Scripted* thermal_ = nullptr;
  sampler_suite::Scripted* power_ = nullptr;
  std::unique_ptr<telemetry::Sampler> sampler_;
};
TYPED_TEST_SUITE_P(SamplerContract);

TYPED_TEST_P(SamplerContract, TheProbeReportsEverySourceAndRegistersWhatIsPresent) {
  this->build();
  const auto report = this->sampler_->probe();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report.value().sources.size(), 2U);
  EXPECT_EQ(report.value().sources[0].source, "thermal");
  EXPECT_EQ(report.value().sources[0].metrics.size(), 3U) << "the absent metric is reported";
  EXPECT_EQ(report.value().sources[1].source, "power");
  EXPECT_EQ(this->timeline_.series,
            (std::vector<telemetry::Metric>{this->thermal_->metric(0), this->thermal_->metric(1),
                                            this->power_->metric(0)}))
      << "present metrics only, in source order";
  const auto again = this->sampler_->probe();
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again.value(), report.value());
  EXPECT_EQ(this->thermal_->probes, 1) << "probed once";
  EXPECT_EQ(this->timeline_.series.size(), 3U) << "and registered once";
}

TYPED_TEST_P(SamplerContract, NothingIsReadBeforeTheProbe) {
  this->build();
  const auto appended = this->sampler_->round(100);
  ASSERT_TRUE(appended.has_value());
  EXPECT_EQ(appended.value(), 0U);
  EXPECT_EQ(this->thermal_->samples, 0);
}

TYPED_TEST_P(SamplerContract, ARoundPutsEachReadingOnItsSeriesAtTheGivenTime) {
  this->build();
  ASSERT_TRUE(this->sampler_->probe().has_value());
  ASSERT_EQ(this->sampler_->round(100).value(), 3U);
  ASSERT_EQ(this->sampler_->round(200).value(), 3U);
  ASSERT_EQ(this->timeline_.points.size(), 3U);
  EXPECT_EQ(this->timeline_.points[1][0], (telemetry::Point{100, telemetry::Gauge{11.0}}));
  EXPECT_EQ(this->timeline_.points[1][1], (telemetry::Point{200, telemetry::Gauge{21.0}}));
  EXPECT_EQ(this->timeline_.points[2][1], (telemetry::Point{200, telemetry::Gauge{20.0}}));
}

TYPED_TEST_P(SamplerContract, ASourceThatBreaksItsContractNeverShiftsAnothersReadings) {
  for (const int extra : {1, -1}) {
    this->build({extra, 0});
    ASSERT_TRUE(this->sampler_->probe().has_value());
    ASSERT_TRUE(this->sampler_->round(100).has_value());
    for (std::size_t series = 0; series < 2; ++series) {
      ASSERT_EQ(this->timeline_.points[series].size(), 1U);
      const auto* unavailable =
          std::get_if<telemetry::Unavailable>(&this->timeline_.points[series][0].reading);
      ASSERT_NE(unavailable, nullptr) << "extra " << extra;
      EXPECT_EQ(unavailable->kind, topology::Availability::kMalformed);
      EXPECT_NE(unavailable->detail.find("thermal"), std::string::npos);
    }
    EXPECT_EQ(this->timeline_.points[2][0], (telemetry::Point{100, telemetry::Gauge{10.0}}))
        << "the well-behaved source is untouched";
    this->timeline_.series.clear();
    this->timeline_.points.clear();
  }
}

TYPED_TEST_P(SamplerContract, ATimelineRefusalEndsTheRound) {
  this->build();
  ASSERT_TRUE(this->sampler_->probe().has_value());
  ASSERT_TRUE(this->sampler_->round(100).has_value());
  const auto stale = this->sampler_->round(100);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().fault, telemetry::TimelineFault::kOutOfOrder);
}

TYPED_TEST_P(SamplerContract, ASeriesTheTimelineRefusesFailsTheProbe) {
  this->build();
  this->timeline_.refuse_series = true;
  const auto report = this->sampler_->probe();
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().fault, telemetry::TimelineFault::kCapacity);
}

REGISTER_TYPED_TEST_SUITE_P(SamplerContract, ASeriesTheTimelineRefusesFailsTheProbe,
                            TheProbeReportsEverySourceAndRegistersWhatIsPresent,
                            NothingIsReadBeforeTheProbe,
                            ARoundPutsEachReadingOnItsSeriesAtTheGivenTime,
                            ASourceThatBreaksItsContractNeverShiftsAnothersReadings,
                            ATimelineRefusalEndsTheRound);

}  // namespace loadforge::contracts

#endif
