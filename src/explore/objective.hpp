// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_EXPLORE_OBJECTIVE_HPP
#define LOADFORGE_EXPLORE_OBJECTIVE_HPP

// An objective: which number of a trial's Load Signature explore is trying to
// make as bad as possible (F5). It is bound against the capability report
// before the first trial, so an objective on a metric this machine cannot read
// fails at configuration time with the reason and the remedy the source gave --
// never by scoring zero. Scoring a signature repeats the rule: a metric with no
// reading in the window is an error, not a value.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "core/result.hpp"
#include "explore/trial.hpp"
#include "statistics/signature.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"

namespace loadforge::explore {

/// What of a gauge's summary is the score.
enum class GaugeStatistic : std::uint8_t { kMean, kMax, kMin, kLast, kStddev, kSlope };

/// What of a counter's summary is the score.
enum class CounterStatistic : std::uint8_t { kRate, kDelta };

[[nodiscard]] constexpr std::string_view describe(GaugeStatistic statistic) noexcept {
  switch (statistic) {
    case GaugeStatistic::kMean:
      return "mean";
    case GaugeStatistic::kMax:
      return "max";
    case GaugeStatistic::kMin:
      return "min";
    case GaugeStatistic::kLast:
      return "last";
    case GaugeStatistic::kStddev:
      return "stddev";
    case GaugeStatistic::kSlope:
      return "slope per second";
  }
  return "unrecognised gauge statistic";
}

[[nodiscard]] constexpr std::string_view describe(CounterStatistic statistic) noexcept {
  switch (statistic) {
    case CounterStatistic::kRate:
      return "rate per second";
    case CounterStatistic::kDelta:
      return "delta";
  }
  return "unrecognised counter statistic";
}

/// Which extreme is the worst case: the highest temperature, or the lowest
/// frequency.
enum class Direction : std::uint8_t { kHigherIsWorse, kLowerIsWorse };

[[nodiscard]] constexpr std::string_view describe(Direction direction) noexcept {
  switch (direction) {
    case Direction::kHigherIsWorse:
      return "higher is worse";
    case Direction::kLowerIsWorse:
      return "lower is worse";
  }
  return "unrecognised direction";
}

/// +1 where higher is worse, -1 where lower is: multiply a value by it and
/// larger means worse.
[[nodiscard]] constexpr double sign_of(Direction direction) noexcept {
  return direction == Direction::kHigherIsWorse ? 1.0 : -1.0;
}

/// The objective as configured.
///
///   metric       the series' name: "thermal.package".
///   scope_index  which of the series of that name (package 1, CPU 3).
///   statistic    a gauge's or a counter's; it must be of the metric's kind.
///   direction    which extreme is the worst case.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct Objective {
  std::string metric;
  std::uint32_t scope_index = 0;
  std::variant<GaugeStatistic, CounterStatistic> statistic = GaugeStatistic::kMean;
  Direction direction = Direction::kHigherIsWorse;
};

/// Why explore could not be configured or could not go on.
///
///   kUnknownMetric      the objective's metric is not in the capability report.
///   kMetricUnavailable  it is, and this machine cannot read it.
///   kStatistic          the statistic is not of the metric's kind.
///   kNoReading          a signature holds no value for the objective's series
///                       -- a sensor that dropped out, a window of one reading
///                       asked for a spread.
///   kConfiguration      the search is not well formed; the detail says how.
///   kDutyCycle          candidates do not all verify at one duty cycle (F17).
///   kTrial              a trial could not be run; `trial` says which fault.
///   kReference          the reference candidate produced no usable reading, so
///                       drift cannot be measured.
enum class ExploreFault : std::uint8_t {
  kUnknownMetric,
  kMetricUnavailable,
  kStatistic,
  kNoReading,
  kConfiguration,
  kDutyCycle,
  kTrial,
  kReference,
};

[[nodiscard]] constexpr std::string_view describe(ExploreFault fault) noexcept {
  switch (fault) {
    case ExploreFault::kUnknownMetric:
      return "objective metric unknown";
    case ExploreFault::kMetricUnavailable:
      return "objective metric unavailable";
    case ExploreFault::kStatistic:
      return "objective statistic does not fit the metric";
    case ExploreFault::kNoReading:
      return "no reading for the objective";
    case ExploreFault::kConfiguration:
      return "search not well formed";
    case ExploreFault::kDutyCycle:
      return "candidates differ in verification duty cycle";
    case ExploreFault::kTrial:
      return "trial failed";
    case ExploreFault::kReference:
      return "reference unusable";
  }
  return "unrecognised explore fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct ExploreError {
  ExploreFault fault = ExploreFault::kConfiguration;
  std::string detail;
  std::optional<TrialFault> trial;  ///< Set for kTrial and kReference from a runner error.
};

/// An objective resolved against a capability report.
struct BoundObjective {
  telemetry::Metric metric;
  std::variant<GaugeStatistic, CounterStatistic> statistic = GaugeStatistic::kMean;
  Direction direction = Direction::kHigherIsWorse;
};

/// Resolves `objective` against `capabilities`: the metric must be in the
/// report, present, and of the statistic's kind. Called before the first
/// trial; the error carries the source's own reason and remedy.
[[nodiscard]] core::Result<BoundObjective, ExploreError> bind(
    const Objective& objective, const telemetry::CapabilityReport& capabilities);

/// The objective's value in `signature`, in the metric's unit. An error
/// rather than a zero when the signature has none.
[[nodiscard]] core::Result<double, ExploreError> score(const BoundObjective& objective,
                                                       const statistics::LoadSignature& signature);

/// An ExploreError with no runner fault behind it.
[[nodiscard]] ExploreError make_error(ExploreFault fault, std::string detail);

}  // namespace loadforge::explore

#endif
