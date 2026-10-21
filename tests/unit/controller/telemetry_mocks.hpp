// SPDX-License-Identifier: GPL-3.0-or-later
//
// The controller's and the command line's own mocks of the telemetry
// contracts they consume (docs/adr/0003): a sampler, a timeline, a source and
// a summarizer, each written from the contract header alone, as read here. The
// provider's suite is run against each of them (contract_mocks_test.cpp), so
// a reading that differs from the provider's shows up as a failing test, not
// as a surprise in the integration tier.

#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_TELEMETRY_MOCKS_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_TELEMETRY_MOCKS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "statistics/signature.hpp"
#include "telemetry/sampler.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::mocks {

/// A timeline held in vectors. Refuses as the contract says, and can be told to
/// refuse a phase or a point on demand, which a real timeline does only at
/// capacity.
class MockTimeline final : public telemetry::Timeline {
 public:
  explicit MockTimeline(std::size_t capacity = 1'000'000) : capacity_(capacity) {}

  core::Result<telemetry::SeriesId, telemetry::TimelineError> add_series(
      const telemetry::Metric& metric) override {
    for (const Series& series : series_) {
      if (telemetry::same_series(series.metric, metric)) {
        return telemetry::TimelineError{telemetry::TimelineFault::kDuplicateSeries, metric.name};
      }
    }
    series_.push_back(Series{metric, {}});
    return static_cast<telemetry::SeriesId>(series_.size() - 1);
  }

  core::Result<core::Ok, telemetry::TimelineError> append(telemetry::SeriesId id,
                                                          const telemetry::Point& point) override {
    if (id >= series_.size()) {
      return telemetry::TimelineError{telemetry::TimelineFault::kUnknownSeries, std::to_string(id)};
    }
    Series& series = series_[id];
    if (!series.points.empty() && point.timestamp_ns <= series.points.back().timestamp_ns) {
      return telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, "not later"};
    }
    if (std::holds_alternative<telemetry::Gauge>(point.reading) &&
        series.metric.kind != telemetry::Kind::kGauge) {
      return telemetry::TimelineError{telemetry::TimelineFault::kKindMismatch, "gauge"};
    }
    if (std::holds_alternative<telemetry::Counter>(point.reading) &&
        series.metric.kind != telemetry::Kind::kCounter) {
      return telemetry::TimelineError{telemetry::TimelineFault::kKindMismatch, "counter"};
    }
    if (total_ >= capacity_ || appends_until_refusal_ == 0) {
      return telemetry::TimelineError{telemetry::TimelineFault::kCapacity, "full"};
    }
    if (appends_until_refusal_ > 0) {
      --appends_until_refusal_;
    }
    series.points.push_back(point);
    ++total_;
    return core::Ok{};
  }

  core::Result<core::Ok, telemetry::TimelineError> mark_phase(telemetry::PhaseMark mark) override {
    if (refuse_phase_) {
      return telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, "scripted"};
    }
    if (!phases_.empty() && mark.start_ns <= phases_.back().start_ns) {
      return telemetry::TimelineError{telemetry::TimelineFault::kOutOfOrder, "not later"};
    }
    phases_.push_back(std::move(mark));
    return core::Ok{};
  }

  [[nodiscard]] std::size_t series_count() const noexcept override { return series_.size(); }
  [[nodiscard]] const telemetry::Metric& metric(telemetry::SeriesId id) const override {
    return series_.at(id).metric;
  }
  [[nodiscard]] std::span<const telemetry::Point> points(telemetry::SeriesId id) const override {
    return series_.at(id).points;
  }
  [[nodiscard]] std::span<const telemetry::PhaseMark> phases() const override { return phases_; }

  /// Accept this many more points, then refuse each with kCapacity.
  void refuse_after(int appends) { appends_until_refusal_ = appends; }
  void refuse_phases() { refuse_phase_ = true; }
  [[nodiscard]] std::size_t total() const { return total_; }

 private:
  struct Series {
    telemetry::Metric metric;
    std::vector<telemetry::Point> points;
  };
  std::size_t capacity_;
  std::size_t total_ = 0;
  int appends_until_refusal_ = -1;
  bool refuse_phase_ = false;
  std::vector<Series> series_;
  std::vector<telemetry::PhaseMark> phases_;
};

/// A sampler as its contract reads here, and a log of what was asked of it. It
/// can be told to answer a round with an error.
class MockSampler final : public telemetry::Sampler {
 public:
  MockSampler(std::vector<std::unique_ptr<telemetry::Source>> sources,
              telemetry::TimelineSink& timeline)
      : sources_(std::move(sources)), timeline_(&timeline) {}

  core::Result<telemetry::CapabilityReport, telemetry::TimelineError> probe() override {
    if (report_.has_value()) {
      return *report_;
    }
    telemetry::CapabilityReport report;
    for (const auto& source : sources_) {
      std::vector<telemetry::SeriesId> ids;
      std::vector<telemetry::MetricStatus> statuses = source->probe();
      for (const telemetry::MetricStatus& status : statuses) {
        if (!status.unavailable.has_value()) {
          auto id = timeline_->add_series(status.metric);
          if (!id.has_value()) {
            return id.error();
          }
          ids.push_back(id.value());
        }
      }
      ids_.push_back(std::move(ids));
      report.sources.push_back({std::string{source->name()}, std::move(statuses)});
    }
    report_ = report;
    return report;
  }

  core::Result<std::size_t, telemetry::TimelineError> round(std::int64_t now_ns) override {
    rounds_asked.push_back(now_ns);
    if (fail_round.has_value()) {
      return *fail_round;
    }
    if (!report_.has_value()) {
      return std::size_t{0};
    }
    std::size_t appended = 0;
    for (std::size_t at = 0; at < sources_.size(); ++at) {
      std::vector<telemetry::Reading> readings;
      sources_[at]->sample(readings);
      if (readings.size() != ids_[at].size()) {
        readings.assign(ids_[at].size(),
                        telemetry::Unavailable{topology::Availability::kMalformed, "",
                                               std::string{sources_[at]->name()} + ": " +
                                                   std::to_string(readings.size()) + " readings, " +
                                                   std::to_string(ids_[at].size()) + " metrics",
                                               "report a bug in the source"});
      }
      for (std::size_t metric = 0; metric < ids_[at].size(); ++metric) {
        auto added = timeline_->append(ids_[at][metric], {now_ns, readings[metric]});
        if (!added.has_value()) {
          return added.error();
        }
        ++appended;
      }
    }
    return appended;
  }

  std::vector<std::int64_t> rounds_asked;
  std::optional<telemetry::TimelineError> fail_round;

 private:
  std::vector<std::unique_ptr<telemetry::Source>> sources_;
  telemetry::TimelineSink* timeline_;
  std::optional<telemetry::CapabilityReport> report_;
  std::vector<std::vector<telemetry::SeriesId>> ids_;
};

/// A sampler that reads nothing: the controller's tests need one that appends
/// a chosen point per round, and a timeline to put it on.
class PointSampler final : public telemetry::Sampler {
 public:
  PointSampler(telemetry::TimelineSink& timeline, telemetry::Metric metric)
      : timeline_(&timeline), metric_(std::move(metric)) {}

  core::Result<telemetry::CapabilityReport, telemetry::TimelineError> probe() override {
    auto id = timeline_->add_series(metric_);
    if (!id.has_value()) {
      return id.error();
    }
    id_ = id.value();
    return telemetry::CapabilityReport{};
  }

  core::Result<std::size_t, telemetry::TimelineError> round(std::int64_t now_ns) override {
    rounds_asked.push_back(now_ns);
    if (fail_from_round.has_value() && rounds_asked.size() >= *fail_from_round) {
      return telemetry::TimelineError{telemetry::TimelineFault::kCapacity, "full"};
    }
    auto added = timeline_->append(id_, {now_ns, value_(rounds_asked.size())});
    if (!added.has_value()) {
      return added.error();
    }
    return std::size_t{1};
  }

  std::vector<std::int64_t> rounds_asked;
  std::optional<std::size_t> fail_from_round;  ///< 1-based: this round and every later one fail.

 private:
  [[nodiscard]] telemetry::Reading value_(std::size_t round) const {
    if (metric_.kind == telemetry::Kind::kCounter) {
      return telemetry::Counter{round * 10};
    }
    return telemetry::Gauge{static_cast<double>(round)};
  }
  telemetry::TimelineSink* timeline_;
  telemetry::Metric metric_;
  telemetry::SeriesId id_ = 0;
};

/// A source that reports what it is told: its statuses at the probe, and per
/// round one reading per present metric.
class MockSource final : public telemetry::Source {
 public:
  MockSource(std::string name, std::vector<telemetry::MetricStatus> statuses,
             std::vector<std::vector<telemetry::Reading>> rounds)
      : name_(std::move(name)), statuses_(std::move(statuses)), rounds_(std::move(rounds)) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  std::vector<telemetry::MetricStatus> probe() override {
    probed_ = true;
    return statuses_;
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    if (!probed_ || rounds_.empty()) {
      return;
    }
    const auto& readings = rounds_[std::min(next_, rounds_.size() - 1)];
    out.insert(out.end(), readings.begin(), readings.end());
    ++next_;
  }

 private:
  std::string name_;
  std::vector<telemetry::MetricStatus> statuses_;
  std::vector<std::vector<telemetry::Reading>> rounds_;
  bool probed_ = false;
  std::size_t next_ = 0;
};

/// A summarizer, written from signature.hpp: plain arithmetic over the present
/// readings of each series in the window.
class MockSummarizer final : public statistics::Summarizer {
 public:
  [[nodiscard]] core::Result<statistics::LoadSignature, statistics::StatisticsError> summarize(
      const telemetry::TimelineView& view, const statistics::Window& window) const override {
    if (window.end_ns <= window.start_ns) {
      return statistics::StatisticsError{statistics::StatisticsFault::kWindow, "empty"};
    }
    statistics::LoadSignature signature;
    signature.window = window;
    for (std::size_t at = 0; at < view.series_count(); ++at) {
      const auto id = static_cast<telemetry::SeriesId>(at);
      signature.metrics.push_back(one(view.metric(id), view.points(id), window));
    }
    return signature;
  }

 private:
  static statistics::MetricSummary one(const telemetry::Metric& metric,
                                       std::span<const telemetry::Point> points,
                                       const statistics::Window& window) {
    statistics::MetricSummary summary;
    summary.metric = metric;
    std::vector<telemetry::Point> inside;
    for (const telemetry::Point& point : points) {
      if (point.timestamp_ns >= window.start_ns && point.timestamp_ns < window.end_ns) {
        inside.push_back(point);
      }
    }
    std::optional<telemetry::Unavailable> last_unavailable;
    std::vector<double> values;
    std::vector<double> times;
    std::uint64_t samples = 0;
    std::uint64_t delta = 0;
    std::uint64_t resets = 0;
    std::int64_t span = 0;
    bool pair = false;
    for (std::size_t at = 0; at < inside.size(); ++at) {
      if (const auto* gone = std::get_if<telemetry::Unavailable>(&inside[at].reading)) {
        ++summary.unavailable_samples;
        last_unavailable = *gone;
        continue;
      }
      ++samples;
      if (const auto* gauge = std::get_if<telemetry::Gauge>(&inside[at].reading)) {
        values.push_back(gauge->value);
        times.push_back(static_cast<double>(inside[at].timestamp_ns - inside.front().timestamp_ns) /
                        1e9);
      } else if (at > 0) {
        const auto* now = std::get_if<telemetry::Counter>(&inside[at].reading);
        const auto* before = std::get_if<telemetry::Counter>(&inside[at - 1].reading);
        if (before != nullptr) {
          const auto advance = telemetry::counter_advance(before->raw, now->raw, metric.modulus);
          if (advance.has_value()) {
            delta += *advance;
            span += inside[at].timestamp_ns - inside[at - 1].timestamp_ns;
            pair = true;
          } else {
            ++resets;
          }
        }
      }
    }
    if (samples == 0) {
      summary.value = last_unavailable.value_or(
          telemetry::Unavailable{topology::Availability::kAbsent, "", "no sample in the window",
                                 "sample for longer or at a shorter interval"});
    } else if (metric.kind == telemetry::Kind::kCounter) {
      statistics::CounterSummary counter;
      counter.samples = samples;
      counter.delta = delta;
      counter.resets = resets;
      if (pair) {
        counter.rate_per_second = static_cast<double>(delta) / (static_cast<double>(span) / 1e9);
      }
      summary.value = counter;
    } else {
      summary.value = gauge(values, times);
    }
    return summary;
  }

  static statistics::GaugeSummary gauge(const std::vector<double>& values,
                                        const std::vector<double>& times) {
    statistics::GaugeSummary gauge;
    gauge.samples = values.size();
    gauge.min = *std::ranges::min_element(values);
    gauge.max = *std::ranges::max_element(values);
    double sum = 0.0;
    for (const double value : values) {
      sum += value;
    }
    const auto n = static_cast<double>(values.size());
    gauge.mean = sum / n;
    gauge.first = values.front();
    gauge.last = values.back();
    if (values.size() > 1) {
      double squares = 0.0;
      for (const double value : values) {
        squares += (value - gauge.mean) * (value - gauge.mean);
      }
      gauge.stddev = std::sqrt(squares / (n - 1.0));
      double mean_time = 0.0;
      for (const double time : times) {
        mean_time += time;
      }
      mean_time /= n;
      double cross = 0.0;
      double spread = 0.0;
      for (std::size_t at = 0; at < values.size(); ++at) {
        cross += (times[at] - mean_time) * (values[at] - gauge.mean);
        spread += (times[at] - mean_time) * (times[at] - mean_time);
      }
      gauge.slope_per_second = cross / spread;
    }
    return gauge;
  }
};

}  // namespace loadforge::mocks

#endif
