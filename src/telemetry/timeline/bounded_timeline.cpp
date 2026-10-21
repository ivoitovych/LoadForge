// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/timeline/bounded_timeline.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace loadforge::telemetry {
namespace {

// Built from a detail the caller made in a statement of its own, so that no
// half-built string is live across a call that may throw (JOURNAL 1.13).
[[nodiscard]] TimelineError refusal(TimelineFault fault, std::string detail) noexcept {
  return TimelineError{fault, std::move(detail)};
}

[[nodiscard]] bool kind_matches(Kind kind, const Reading& reading) noexcept {
  if (std::get_if<Gauge>(&reading) != nullptr) {
    return kind == Kind::kGauge;
  }
  if (std::get_if<Counter>(&reading) != nullptr) {
    return kind == Kind::kCounter;
  }
  return true;  // an Unavailable is accepted for any series
}

[[nodiscard]] std::size_t saturating_multiply(std::size_t lhs, std::size_t rhs) noexcept {
  std::size_t product = 0;
  if (__builtin_mul_overflow(lhs, rhs, &product)) {
    return std::numeric_limits<std::size_t>::max();
  }
  return product;
}

}  // namespace

core::Result<SeriesId, TimelineError> BoundedTimeline::add_series(const Metric& metric) {
  for (const Series& existing : series_) {
    if (same_series(existing.metric, metric)) {
      std::string detail = "a series named " + metric.name + " already exists";
      return refusal(TimelineFault::kDuplicateSeries, std::move(detail));
    }
  }
  series_.emplace_back(metric);
  return static_cast<SeriesId>(series_.size() - 1);
}

core::Result<core::Ok, TimelineError> BoundedTimeline::append(SeriesId series, const Point& point) {
  if (series >= series_.size()) {
    std::string detail = "no series " + std::to_string(series);
    return refusal(TimelineFault::kUnknownSeries, std::move(detail));
  }
  Series& target = series_[series];
  if (!target.points.empty() && point.timestamp_ns <= target.points.back().timestamp_ns) {
    std::string detail = "timestamp " + std::to_string(point.timestamp_ns) + " is not later than " +
                         std::to_string(target.points.back().timestamp_ns);
    return refusal(TimelineFault::kOutOfOrder, std::move(detail));
  }
  if (!kind_matches(target.metric.kind, point.reading)) {
    std::string detail = "the reading does not suit " + target.metric.name;
    return refusal(TimelineFault::kKindMismatch, std::move(detail));
  }
  if (size_ >= capacity_) {
    std::string detail = "the timeline holds " + std::to_string(capacity_) + " points";
    return refusal(TimelineFault::kCapacity, std::move(detail));
  }
  target.points.push_back(point);
  ++size_;
  return core::Ok{};
}

core::Result<core::Ok, TimelineError> BoundedTimeline::mark_phase(PhaseMark mark) {
  if (!phases_.empty() && mark.start_ns <= phases_.back().start_ns) {
    std::string detail = "phase " + mark.label + " starts at " + std::to_string(mark.start_ns) +
                         ", not later than " + std::to_string(phases_.back().start_ns);
    return refusal(TimelineFault::kOutOfOrder, std::move(detail));
  }
  phases_.push_back(std::move(mark));
  return core::Ok{};
}

std::size_t BoundedTimeline::reserved() const noexcept {
  std::size_t total = 0;
  for (const Series& series : series_) {
    total += series.points.capacity();
  }
  return total;
}

std::size_t run_capacity(std::size_t seconds, std::size_t hertz, std::size_t series) noexcept {
  return saturating_multiply(saturating_multiply(seconds, hertz), series);
}

std::size_t memory_bound_bytes(std::size_t capacity) noexcept {
  return saturating_multiply(saturating_multiply(capacity, sizeof(Point)), 3);
}

}  // namespace loadforge::telemetry
