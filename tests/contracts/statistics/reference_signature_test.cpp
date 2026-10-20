// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference summarizer, run against the signature suite, and the
// definitions src/statistics/signature.hpp carries.

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/statistics/signature_contract.hpp"
#include "statistics/signature.hpp"

namespace loadforge::contracts {
namespace {

using statistics::CounterSummary;
using statistics::GaugeSummary;
using statistics::LoadSignature;
using statistics::MetricSummary;
using statistics::Window;
using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Unavailable;

/// Neumaier's compensated sum, in the order the values arrive.
class Sum {
 public:
  void add(double value) {
    const double total = sum_ + value;
    compensation_ +=
        std::abs(sum_) >= std::abs(value) ? (sum_ - total) + value : (value - total) + sum_;
    sum_ = total;
  }
  [[nodiscard]] double value() const { return sum_ + compensation_; }

 private:
  double sum_ = 0.0;
  double compensation_ = 0.0;
};

GaugeSummary gauge_summary(const std::vector<std::pair<std::int64_t, double>>& readings) {
  GaugeSummary summary;
  summary.samples = readings.size();
  summary.first = readings.front().second;
  summary.last = readings.back().second;
  summary.min = summary.max = summary.first;
  Sum sum;
  for (const auto& [time, value] : readings) {
    summary.min = std::min(summary.min, value);
    summary.max = std::max(summary.max, value);
    sum.add(value);
  }
  const auto n = static_cast<double>(readings.size());
  summary.mean = sum.value() / n;
  if (readings.size() >= 2) {
    Sum squares;
    Sum times;
    for (const auto& [time, value] : readings) {
      squares.add((value - summary.mean) * (value - summary.mean));
      times.add(static_cast<double>(time - readings.front().first) / 1e9);
    }
    summary.stddev = std::sqrt(squares.value() / (n - 1));
    const double mean_time = times.value() / n;
    Sum covariance;
    Sum variance;
    for (const auto& [time, value] : readings) {
      const double t = static_cast<double>(time - readings.front().first) / 1e9 - mean_time;
      covariance.add(t * (value - summary.mean));
      variance.add(t * t);
    }
    if (variance.value() > 0.0) {
      summary.slope_per_second = covariance.value() / variance.value();
    }
  }
  return summary;
}

class ReferenceSummarizerImpl final : public statistics::Summarizer {
 public:
  [[nodiscard]] core::Result<LoadSignature, statistics::StatisticsError> summarize(
      const telemetry::TimelineView& view, const Window& window) const override {
    if (window.end_ns <= window.start_ns) {
      return statistics::StatisticsError{statistics::StatisticsFault::kWindow, "empty window"};
    }
    LoadSignature signature{window, {}};
    for (telemetry::SeriesId id = 0; id < view.series_count(); ++id) {
      MetricSummary summary{view.metric(id),
                            Unavailable{topology::Availability::kAbsent, "",
                                        "no sample in the window", "sample longer, or more often"},
                            0};
      std::vector<std::pair<std::int64_t, double>> gauges;
      CounterSummary counter;
      std::optional<std::pair<std::int64_t, std::uint64_t>> previous;
      std::int64_t span_ns = 0;
      for (const telemetry::Point& point : view.points(id)) {
        if (point.timestamp_ns < window.start_ns || point.timestamp_ns >= window.end_ns) {
          continue;
        }
        if (const auto* gauge = std::get_if<Gauge>(&point.reading)) {
          gauges.emplace_back(point.timestamp_ns, gauge->value);
        } else if (const auto* raw = std::get_if<Counter>(&point.reading)) {
          ++counter.samples;
          if (previous.has_value()) {
            counter.delta +=
                telemetry::counter_delta(previous->second, raw->raw, summary.metric.modulus);
            span_ns += point.timestamp_ns - previous->first;
          }
          previous = std::make_pair(point.timestamp_ns, raw->raw);
        } else {
          ++summary.unavailable_samples;
          previous.reset();
          if (gauges.empty() && counter.samples == 0) {
            summary.value = std::get<Unavailable>(point.reading);
          }
        }
      }
      if (!gauges.empty()) {
        summary.value = gauge_summary(gauges);
      } else if (counter.samples > 0) {
        if (span_ns > 0) {
          counter.rate_per_second =
              static_cast<double>(counter.delta) / (static_cast<double>(span_ns) / 1e9);
        }
        summary.value = counter;
      }
      signature.metrics.push_back(summary);
    }
    return signature;
  }
};

struct ReferenceSummarizer {
  static std::unique_ptr<statistics::Summarizer> make() {
    return std::make_unique<ReferenceSummarizerImpl>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, SummarizerContract,
                               ::testing::Types<ReferenceSummarizer>);

TEST(SignatureDefinition, FaultNamesAndMemberwiseEquality) {
  EXPECT_EQ(statistics::describe(statistics::StatisticsFault::kWindow), "empty or inverted window");
  EXPECT_EQ(statistics::describe(static_cast<statistics::StatisticsFault>(9)),
            "unrecognised statistics fault");
  expect_memberwise_equality(Window{1, 2},
                             {[](Window& w) { w.start_ns = 0; }, [](Window& w) { w.end_ns = 3; }});
  expect_memberwise_equality(
      GaugeSummary{1, 2, 3, 4, 5, 6, 7, 8},
      {[](GaugeSummary& g) { g.samples = 9; }, [](GaugeSummary& g) { g.min = 9; },
       [](GaugeSummary& g) { g.max = 9; }, [](GaugeSummary& g) { g.mean = 9; },
       [](GaugeSummary& g) { g.stddev.reset(); }, [](GaugeSummary& g) { g.first = 9; },
       [](GaugeSummary& g) { g.last = 9; }, [](GaugeSummary& g) { g.slope_per_second.reset(); }});
  expect_memberwise_equality(
      CounterSummary{1, 2, 3.0},
      {[](CounterSummary& c) { c.samples = 9; }, [](CounterSummary& c) { c.delta = 9; },
       [](CounterSummary& c) { c.rate_per_second.reset(); }});
}

}  // namespace
}  // namespace loadforge::contracts
