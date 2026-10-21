// SPDX-License-Identifier: GPL-3.0-or-later
//
// The round sampler: the contract's suite against it; the source suite
// against this file's own mock of a source (ADR 0003); and what the suite
// leaves open -- order across sources, a sampler that is probed twice, a
// timeline that refuses mid-round or at the probe, and sources of every
// shape of breach.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/telemetry/sampler_contract.hpp"
#include "contracts/telemetry/telemetry_contract.hpp"
#include "telemetry/sampler.hpp"
#include "telemetry/timeline/bounded_timeline.hpp"

namespace loadforge::telemetry {
namespace {

using topology::Availability;

Metric gauge_metric(const std::string& name) {
  return Metric{name, Unit::kCelsius, Kind::kGauge, Scope::kPackage, 0, std::nullopt, "test"};
}

Metric counter_metric(const std::string& name) {
  return Metric{name, Unit::kJoules, Kind::kCounter, Scope::kPackage, 0, 1000, "test"};
}

Unavailable absent_status() {
  return Unavailable{Availability::kAbsent, "/sys/none", "this machine has none",
                     "nothing to change"};
}

/// A source as its contract reads: a gauge, a wrapping counter and a metric
/// this machine does not have; or, when `all_absent`, only the last. Each
/// round the gauge rises, the counter wraps, and in "vanishing" mode the
/// gauge is gone on odd rounds.
class MockSource final : public Source {
 public:
  enum class Mode : std::uint8_t { kPresent, kAllAbsent, kVanishing };

  MockSource(std::string name, Mode mode) : name_(std::move(name)), mode_(mode) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }

  std::vector<MetricStatus> probe() override {
    probed_ = true;
    std::vector<MetricStatus> statuses;
    if (mode_ != Mode::kAllAbsent) {
      statuses.push_back({gauge_metric(name_ + ".level"), std::nullopt});
      statuses.push_back({counter_metric(name_ + ".energy"), std::nullopt});
    }
    statuses.push_back({gauge_metric(name_ + ".missing"), absent_status()});
    return statuses;
  }

  void sample(std::vector<Reading>& out) override {
    if (!probed_ || mode_ == Mode::kAllAbsent) {
      return;
    }
    ++round_;
    if (mode_ == Mode::kVanishing && round_ % 2 == 1) {
      out.emplace_back(
          Unavailable{Availability::kVanished, "/sys/level", "it went", "re-plug the sensor"});
    } else {
      out.emplace_back(Gauge{40.0 + static_cast<double>(round_)});
    }
    out.emplace_back(Counter{(round_ * 400U) % 1000U});
  }

 private:
  std::string name_;
  Mode mode_;
  bool probed_ = false;
  std::uint64_t round_ = 0;
};

template <MockSource::Mode M>
struct MockSubject {
  static constexpr int kRounds = 5;
  static std::unique_ptr<Source> make() { return std::make_unique<MockSource>("thermal", M); }
};

struct SamplerSubject {
  static std::unique_ptr<Sampler> make(std::vector<std::unique_ptr<Source>> sources,
                                       TimelineSink& timeline) {
    return make_sampler(std::move(sources), timeline);
  }
};

using MockSources = ::testing::Types<MockSubject<MockSource::Mode::kPresent>,
                                     MockSubject<MockSource::Mode::kAllAbsent>,
                                     MockSubject<MockSource::Mode::kVanishing>>;

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

INSTANTIATE_TYPED_TEST_SUITE_P(RoundSampler, SamplerContract,
                               ::testing::Types<telemetry::SamplerSubject>);
INSTANTIATE_TYPED_TEST_SUITE_P(Mock, SourceContract, telemetry::MockSources);
// The timeline's suite belongs to the timeline's tests.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);

}  // namespace loadforge::contracts

namespace loadforge::telemetry {
namespace {

/// Appends `count` readings per sample() of one fixed reading, whatever the
/// metrics are: the shape of breach is the test's choice.
class Breaching final : public Source {
 public:
  Breaching(std::string name, std::size_t present, std::size_t appends)
      : name_(std::move(name)), present_(present), appends_(appends) {}
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  std::vector<MetricStatus> probe() override {
    std::vector<MetricStatus> statuses;
    for (std::size_t at = 0; at < present_; ++at) {
      statuses.push_back({gauge_metric(name_ + ".m" + std::to_string(at)), std::nullopt});
    }
    if (present_ == 0) {
      statuses.push_back({gauge_metric(name_ + ".none"), absent_status()});
    }
    return statuses;
  }
  void sample(std::vector<Reading>& out) override {
    for (std::size_t at = 0; at < appends_; ++at) {
      out.emplace_back(Gauge{1.0});
    }
  }

 private:
  std::string name_;
  std::size_t present_;
  std::size_t appends_;
};

/// Records the order sources are probed in.
class Ordered final : public Source {
 public:
  Ordered(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(&log) {}
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  std::vector<MetricStatus> probe() override {
    log_->push_back("probe " + name_);
    return {{gauge_metric(name_ + ".m"), std::nullopt}};
  }
  void sample(std::vector<Reading>& out) override {
    log_->push_back("sample " + name_);
    out.emplace_back(Gauge{1.0});
  }

 private:
  std::string name_;
  std::vector<std::string>* log_;
};

std::vector<std::unique_ptr<Source>> sources_of(std::unique_ptr<Source> a,
                                                std::unique_ptr<Source> b = nullptr) {
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::move(a));
  if (b != nullptr) {
    sources.push_back(std::move(b));
  }
  return sources;
}

TEST(RoundSampler, ProbesAndSamplesInSourceOrder) {
  std::vector<std::string> log;
  BoundedTimeline timeline(100);
  const auto sampler = make_sampler(sources_of(std::make_unique<Ordered>("thermal", log),
                                               std::make_unique<Ordered>("power", log)),
                                    timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  ASSERT_EQ(sampler->round(10).value(), 2U);
  EXPECT_EQ(log, (std::vector<std::string>{"probe thermal", "probe power", "sample thermal",
                                           "sample power"}));
  EXPECT_EQ(timeline.metric(0).name, "thermal.m");
  EXPECT_EQ(timeline.metric(1).name, "power.m");
}

TEST(RoundSampler, NoSourcesIsAnEmptyReportAndEmptyRounds) {
  BoundedTimeline timeline(10);
  const auto sampler = make_sampler({}, timeline);
  const auto report = sampler->probe();
  ASSERT_TRUE(report.has_value());
  EXPECT_TRUE(report.value().sources.empty());
  EXPECT_EQ(sampler->round(1).value(), 0U);
}

TEST(RoundSampler, TheReportCarriesAbsenceWithItsRemedyAndCountsMatchTheSeries) {
  BoundedTimeline timeline(100);
  const auto sampler =
      make_sampler(sources_of(std::make_unique<MockSource>("thermal", MockSource::Mode::kPresent),
                              std::make_unique<MockSource>("fan", MockSource::Mode::kAllAbsent)),
                   timeline);
  const auto report = sampler->probe();
  ASSERT_TRUE(report.has_value());
  const CapabilityCounts counts = count(report.value());
  EXPECT_EQ(counts.metrics, 4U);
  EXPECT_EQ(counts.present, 2U);
  EXPECT_EQ(timeline.series_count(), counts.present);
  const auto& fan = report.value().sources[1];
  ASSERT_TRUE(fan.metrics[0].unavailable.has_value());
  EXPECT_EQ(fan.metrics[0].unavailable->remediation, "nothing to change");
  EXPECT_EQ(sampler->round(1).value(), 2U) << "a source with nothing present adds no points";
}

TEST(RoundSampler, AVanishedReadingIsRecordedAsOneAndTheCounterIsKept) {
  BoundedTimeline timeline(100);
  const auto sampler = make_sampler(
      sources_of(std::make_unique<MockSource>("thermal", MockSource::Mode::kVanishing)), timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  ASSERT_TRUE(sampler->round(1).has_value());
  ASSERT_TRUE(sampler->round(2).has_value());
  const Unavailable* vanished = std::get_if<Unavailable>(&timeline.points(0)[0].reading);
  ASSERT_NE(vanished, nullptr);
  EXPECT_EQ(vanished->kind, Availability::kVanished);
  EXPECT_EQ(timeline.points(0)[1], (Point{2, Gauge{42.0}}));
  EXPECT_EQ(timeline.points(1)[1], (Point{2, Counter{800}}));
}

TEST(RoundSampler, EveryShapeOfBreachBecomesMalformedNamingBothCounts) {
  struct Case {
    std::size_t present;
    std::size_t appends;
    std::string counts;
  };
  const Case cases[] = {{2, 3, "3 readings for 2 present"},
                        {2, 1, "1 readings for 2 present"},
                        {2, 0, "0 readings for 2 present"},
                        {0, 1, "1 readings for 0 present"}};
  for (const Case& c : cases) {
    BoundedTimeline timeline(100);
    const auto sampler = make_sampler(
        sources_of(std::make_unique<Breaching>("fan", c.present, c.appends),
                   std::make_unique<MockSource>("thermal", MockSource::Mode::kPresent)),
        timeline);
    ASSERT_TRUE(sampler->probe().has_value());
    const auto appended = sampler->round(5);
    ASSERT_TRUE(appended.has_value());
    EXPECT_EQ(appended.value(), c.present + 2) << c.counts;
    for (SeriesId id = 0; id < c.present; ++id) {
      const Unavailable* bad = std::get_if<Unavailable>(&timeline.points(id)[0].reading);
      ASSERT_NE(bad, nullptr);
      EXPECT_EQ(bad->kind, Availability::kMalformed);
      EXPECT_NE(bad->detail.find("fan returned " + c.counts), std::string::npos) << bad->detail;
      EXPECT_FALSE(bad->remediation.empty());
    }
    const auto good = static_cast<SeriesId>(c.present);
    EXPECT_EQ(timeline.points(good)[0], (Point{5, Gauge{41.0}}));
  }
}

TEST(RoundSampler, ASeriesRefusedAtTheProbeIsTheAnswerEveryTimeAndNothingIsRead) {
  std::vector<std::string> log;
  BoundedTimeline timeline(100);
  ASSERT_TRUE(timeline.add_series(gauge_metric("power.m")).has_value());  // taken already
  const auto sampler = make_sampler(sources_of(std::make_unique<Ordered>("thermal", log),
                                               std::make_unique<Ordered>("power", log)),
                                    timeline);
  const auto first = sampler->probe();
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().fault, TimelineFault::kDuplicateSeries);
  const auto second = sampler->probe();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), first.error());
  EXPECT_EQ(log, (std::vector<std::string>{"probe thermal", "probe power"})) << "probed once";
  EXPECT_EQ(sampler->round(1).value(), 0U) << "a sampler that failed to probe reads nothing";
  EXPECT_EQ(timeline.series_count(), 2U) << "thermal.m registered; nothing after the refusal";
}

TEST(RoundSampler, ATimelineThatFillsMidRoundEndsTheRoundAndKeepsWhatWasAppended) {
  BoundedTimeline timeline(4);
  const auto sampler =
      make_sampler(sources_of(std::make_unique<MockSource>("thermal", MockSource::Mode::kPresent),
                              std::make_unique<MockSource>("power", MockSource::Mode::kPresent)),
                   timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  ASSERT_EQ(sampler->round(1).value(), 4U);
  const auto full = sampler->round(2);
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error().fault, TimelineFault::kCapacity);
  EXPECT_EQ(timeline.size(), 4U);
}

TEST(RoundSampler, ATimelineThatFillsPartWayThroughARoundKeepsTheEarlierPoints) {
  BoundedTimeline timeline(5);
  const auto sampler =
      make_sampler(sources_of(std::make_unique<MockSource>("thermal", MockSource::Mode::kPresent),
                              std::make_unique<MockSource>("power", MockSource::Mode::kPresent)),
                   timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  ASSERT_EQ(sampler->round(1).value(), 4U);
  EXPECT_FALSE(sampler->round(2).has_value());
  EXPECT_EQ(timeline.size(), 5U) << "the first point of the refused round stayed";
}

}  // namespace
}  // namespace loadforge::telemetry
