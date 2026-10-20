// SPDX-License-Identifier: GPL-3.0-or-later
#include "statistics/summarizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace loadforge::statistics {
namespace {

constexpr double kNanosecondsPerSecond = 1e9;

/// Neumaier's compensated sum, in the order the values arrive.
class Sum {
 public:
  void add(double value) noexcept {
    const double total = sum_ + value;
    compensation_ +=
        std::abs(sum_) >= std::abs(value) ? (sum_ - total) + value : (value - total) + sum_;
    sum_ = total;
  }
  [[nodiscard]] double value() const noexcept { return sum_ + compensation_; }

 private:
  double sum_ = 0.0;
  double compensation_ = 0.0;
};

struct GaugeReading {
  std::int64_t timestamp_ns;
  double value;
};

GaugeSummary summarize_gauge(const std::vector<GaugeReading>& readings) {
  GaugeSummary summary;
  summary.samples = readings.size();
  summary.first = readings.front().value;
  summary.last = readings.back().value;
  summary.min = summary.first;
  summary.max = summary.first;
  Sum values;
  for (const GaugeReading& reading : readings) {
    summary.min = std::min(summary.min, reading.value);
    summary.max = std::max(summary.max, reading.value);
    values.add(reading.value);
  }
  const auto count = static_cast<double>(readings.size());
  summary.mean = values.value() / count;
  if (readings.size() < 2) {
    return summary;
  }
  const std::int64_t origin = readings.front().timestamp_ns;
  Sum squares;
  Sum times;
  for (const GaugeReading& reading : readings) {
    const double deviation = reading.value - summary.mean;
    squares.add(deviation * deviation);
    times.add(static_cast<double>(reading.timestamp_ns - origin) / kNanosecondsPerSecond);
  }
  summary.stddev = std::sqrt(squares.value() / (count - 1.0));
  const double mean_time = times.value() / count;
  Sum covariance;
  Sum variance;
  for (const GaugeReading& reading : readings) {
    const double time =
        static_cast<double>(reading.timestamp_ns - origin) / kNanosecondsPerSecond - mean_time;
    covariance.add(time * (reading.value - summary.mean));
    variance.add(time * time);
  }
  // Timestamps are strictly increasing in a timeline, so the variance of time
  // is positive; a view that repeats one timestamp throughout has no slope.
  if (variance.value() > 0.0) {
    summary.slope_per_second = covariance.value() / variance.value();
  }
  return summary;
}

telemetry::Unavailable nothing_in_window() {
  return {topology::Availability::kAbsent, "", "no sample in the window",
          "sample for longer, or more often, so that the window holds a reading"};
}

MetricSummary summarize_series(const telemetry::Metric& metric,
                               std::span<const telemetry::Point> points) {
  MetricSummary summary{metric, {}, 0};
  std::vector<GaugeReading> gauges;
  CounterSummary counter;
  struct Seen {
    std::int64_t timestamp_ns;
    std::uint64_t raw;
  };
  std::optional<Seen> previous;  // the last present counter reading, if no gap since
  std::int64_t span_ns = 0;
  const telemetry::Unavailable* last_unavailable = nullptr;
  for (const telemetry::Point& point : points) {
    if (const auto* gone = std::get_if<telemetry::Unavailable>(&point.reading)) {
      ++summary.unavailable_samples;
      last_unavailable = gone;
      previous.reset();
    } else if (const auto* gauge = std::get_if<telemetry::Gauge>(&point.reading)) {
      if (metric.kind == telemetry::Kind::kGauge) {
        gauges.push_back({point.timestamp_ns, gauge->value});
      }
    } else if (metric.kind == telemetry::Kind::kCounter) {
      const auto& raw = std::get<telemetry::Counter>(point.reading);
      ++counter.samples;
      if (previous.has_value()) {
        counter.delta += telemetry::counter_delta(previous->raw, raw.raw, metric.modulus);
        span_ns += point.timestamp_ns - previous->timestamp_ns;
      }
      previous = Seen{point.timestamp_ns, raw.raw};
    }
  }
  if (!gauges.empty()) {
    summary.value = summarize_gauge(gauges);
  } else if (counter.samples > 0) {
    if (span_ns > 0) {
      counter.rate_per_second = static_cast<double>(counter.delta) /
                                (static_cast<double>(span_ns) / kNanosecondsPerSecond);
    }
    summary.value = counter;
  } else if (last_unavailable != nullptr) {
    summary.value = *last_unavailable;
  } else {
    summary.value = nothing_in_window();
  }
  return summary;
}

}  // namespace

core::Result<LoadSignature, StatisticsError> CompensatedSummarizer::summarize(
    const telemetry::TimelineView& view, const Window& window) const {
  if (window.end_ns <= window.start_ns) {
    return StatisticsError{StatisticsFault::kWindow, "the window ends no later than it starts"};
  }
  LoadSignature signature{window, {}};
  const std::size_t count = view.series_count();
  signature.metrics.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto id = static_cast<telemetry::SeriesId>(index);
    const std::span<const telemetry::Point> all = view.points(id);
    const auto first = std::lower_bound(all.begin(), all.end(), window.start_ns,
                                        [](const telemetry::Point& point, std::int64_t bound) {
                                          return point.timestamp_ns < bound;
                                        });
    const auto last = std::lower_bound(first, all.end(), window.end_ns,
                                       [](const telemetry::Point& point, std::int64_t bound) {
                                         return point.timestamp_ns < bound;
                                       });
    signature.metrics.push_back(
        summarize_series(view.metric(id), std::span<const telemetry::Point>(first, last)));
  }
  return signature;
}

}  // namespace loadforge::statistics
