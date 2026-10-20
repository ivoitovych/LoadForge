// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/objective.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace loadforge::explore {

namespace {

std::optional<double> gauge_value(GaugeStatistic statistic,
                                  const statistics::GaugeSummary& gauge) noexcept {
  switch (statistic) {
    case GaugeStatistic::kMean:
      return gauge.mean;
    case GaugeStatistic::kMax:
      return gauge.max;
    case GaugeStatistic::kMin:
      return gauge.min;
    case GaugeStatistic::kLast:
      return gauge.last;
    case GaugeStatistic::kStddev:
      return gauge.stddev;
    case GaugeStatistic::kSlope:
      return gauge.slope_per_second;
  }
  return std::nullopt;
}

std::optional<double> counter_value(CounterStatistic statistic,
                                    const statistics::CounterSummary& counter) noexcept {
  switch (statistic) {
    case CounterStatistic::kRate:
      return counter.rate_per_second;
    case CounterStatistic::kDelta:
      return static_cast<double>(counter.delta);
  }
  return std::nullopt;
}

std::string statistic_name(const std::variant<GaugeStatistic, CounterStatistic>& statistic) {
  if (const auto* gauge = std::get_if<GaugeStatistic>(&statistic)) {
    return std::string(describe(*gauge));
  }
  return std::string(describe(std::get<CounterStatistic>(statistic)));
}

}  // namespace

ExploreError make_error(ExploreFault fault, std::string detail) {
  return ExploreError{fault, std::move(detail), std::nullopt};
}

core::Result<BoundObjective, ExploreError> bind(const Objective& objective,
                                                const telemetry::CapabilityReport& capabilities) {
  for (const telemetry::SourceStatus& source : capabilities.sources) {
    for (const telemetry::MetricStatus& status : source.metrics) {
      if (status.metric.name != objective.metric ||
          status.metric.scope_index != objective.scope_index) {
        continue;
      }
      if (status.unavailable.has_value()) {
        std::string detail = "objective metric " + objective.metric +
                             " is unavailable: " + status.unavailable->detail + " (" +
                             status.unavailable->remediation + ")";
        return make_error(ExploreFault::kMetricUnavailable, std::move(detail));
      }
      const bool wants_gauge = std::holds_alternative<GaugeStatistic>(objective.statistic);
      if (wants_gauge != (status.metric.kind == telemetry::Kind::kGauge)) {
        std::string detail = "statistic " + statistic_name(objective.statistic) +
                             " does not fit the kind of " + objective.metric;
        return make_error(ExploreFault::kStatistic, std::move(detail));
      }
      return BoundObjective{status.metric, objective.statistic, objective.direction};
    }
  }
  std::string detail = "objective metric " + objective.metric + " (index " +
                       std::to_string(objective.scope_index) + ") is not in the capability report";
  return make_error(ExploreFault::kUnknownMetric, std::move(detail));
}

core::Result<double, ExploreError> score(const BoundObjective& objective,
                                         const statistics::LoadSignature& signature) {
  for (const statistics::MetricSummary& summary : signature.metrics) {
    if (!telemetry::same_series(summary.metric, objective.metric)) {
      continue;
    }
    std::optional<double> value;
    if (const auto* gauge = std::get_if<statistics::GaugeSummary>(&summary.value)) {
      const auto* statistic = std::get_if<GaugeStatistic>(&objective.statistic);
      if (statistic == nullptr) {
        return make_error(ExploreFault::kStatistic,
                          "the signature holds a gauge for " + objective.metric.name);
      }
      value = gauge_value(*statistic, *gauge);
    } else if (const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value)) {
      const auto* statistic = std::get_if<CounterStatistic>(&objective.statistic);
      if (statistic == nullptr) {
        return make_error(ExploreFault::kStatistic,
                          "the signature holds a counter for " + objective.metric.name);
      }
      value = counter_value(*statistic, *counter);
    } else {
      const auto& gone = std::get<telemetry::Unavailable>(summary.value);
      return make_error(ExploreFault::kNoReading,
                        objective.metric.name + " has no reading in the window: " + gone.detail +
                            " (" + gone.remediation + ")");
    }
    if (!value.has_value()) {
      return make_error(ExploreFault::kNoReading, objective.metric.name +
                                                      " has too few readings for " +
                                                      statistic_name(objective.statistic));
    }
    return *value;
  }
  return make_error(ExploreFault::kNoReading,
                    "the signature has no series " + objective.metric.name);
}

}  // namespace loadforge::explore
