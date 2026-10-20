// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_TIMELINE_HPP
#define LOADFORGE_TELEMETRY_TIMELINE_HPP

// CONTRACT (ADR 0004). The common monotonic timeline every reading lands on
// (IMPLEMENTATION §3, telemetry/timeline), and the phase marks that let a
// reading be attributed to what the machine was doing (PLAN F13: an ECC delta
// is reported per phase). Frozen: changed only by a contract change, never
// inside a stream.
//
//   Provider: the telemetry-pipeline stream (src/telemetry/timeline/).
//   Writer: the sampler, through TimelineSink.
//   Readers: statistics, results, the crash journal, the controller when a
//     worker dies (the seconds before a fault are the most valuable output
//     this tool has), through TimelineView.
//
// TIME
// ----
// Every timestamp is CLOCK_MONOTONIC in nanoseconds: the clock heartbeats
// already use (worker/ipc/heartbeat.hpp), shared by every process on the
// machine and never stepped.
//
// THE SAMPLER'S SIDE OF THE CONTRACT
// ----------------------------------
// A sampler reads each source at a fixed interval, stamps the readings with
// one clock reading per round, and appends them. It reads a wrapping counter
// at least twice per wrap period (sample.hpp, counter_delta), so a wrap is
// never mistaken for a smaller advance. It records its interval, because two
// runs sampled at different rates are not comparable (F17).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

#include "core/result.hpp"
#include "telemetry/sample.hpp"

namespace loadforge::telemetry {

/// A series' handle: its position in the order the series were added,
/// from 0.
using SeriesId = std::uint32_t;

struct Point {
  std::int64_t timestamp_ns = 0;
  Reading reading;
};

[[nodiscard]] inline bool operator==(const Point& lhs, const Point& rhs) noexcept {
  return lhs.timestamp_ns == rhs.timestamp_ns && same_reading(lhs.reading, rhs.reading);
}

/// What the machine was doing from `start_ns` until the next mark: a phase of
/// a schedule, a workload, "idle", "settle".
struct PhaseMark {
  std::int64_t start_ns = 0;
  std::string label;
};

[[nodiscard]] inline bool operator==(const PhaseMark& lhs, const PhaseMark& rhs) noexcept {
  return std::tie(lhs.start_ns, lhs.label) == std::tie(rhs.start_ns, rhs.label);
}

/// Why the timeline refused.
///
///   kDuplicateSeries  a series with that metric's identity already exists.
///   kUnknownSeries    no series has that id.
///   kOutOfOrder       a timestamp not later than the series' last one, or a
///                     phase not later than the last phase.
///   kKindMismatch     a Gauge for a counter metric, or a Counter for a gauge.
///   kCapacity         the timeline holds as many points as it was built for.
///                     Nothing is dropped to make room: a timeline that
///                     silently lost its oldest minutes would lose exactly
///                     the context a long run's report needs.
enum class TimelineFault : std::uint8_t {
  kDuplicateSeries,
  kUnknownSeries,
  kOutOfOrder,
  kKindMismatch,
  kCapacity,
};

[[nodiscard]] constexpr std::string_view describe(TimelineFault fault) noexcept {
  switch (fault) {
    case TimelineFault::kDuplicateSeries:
      return "duplicate series";
    case TimelineFault::kUnknownSeries:
      return "unknown series";
    case TimelineFault::kOutOfOrder:
      return "out of order";
    case TimelineFault::kKindMismatch:
      return "reading of the wrong kind";
    case TimelineFault::kCapacity:
      return "capacity reached";
  }
  return "unrecognised timeline fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct TimelineError {
  TimelineFault fault = TimelineFault::kUnknownSeries;
  std::string detail;
};

[[nodiscard]] inline bool operator==(const TimelineError& lhs, const TimelineError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// The writing side. Every refusal leaves the timeline as it was.
///
/// add_series(metric) registers a series and answers its id, the next in
/// sequence; kDuplicateSeries when same_series() holds with an existing one.
/// append(id, point) adds a point; kUnknownSeries, kOutOfOrder (timestamp not
/// strictly greater than the series' last), kKindMismatch, kCapacity (the
/// total of points over all series has reached the capacity the timeline was
/// built with). An Unavailable point is accepted for any series.
/// mark_phase(mark) starts a phase; kOutOfOrder unless strictly later than
/// the last mark.
class TimelineSink {
 public:
  TimelineSink() = default;
  TimelineSink(const TimelineSink&) = delete;
  TimelineSink(TimelineSink&&) = delete;
  TimelineSink& operator=(const TimelineSink&) = delete;
  TimelineSink& operator=(TimelineSink&&) = delete;
  virtual ~TimelineSink() = default;

  [[nodiscard]] virtual core::Result<SeriesId, TimelineError> add_series(const Metric& metric) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, TimelineError> append(SeriesId series,
                                                                     const Point& point) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, TimelineError> mark_phase(PhaseMark mark) = 0;
};

/// The reading side.
///
/// series_count() is the number of series; metric(id) and points(id) are
/// defined for id below it, and points(id) are every point appended to that
/// series, in order. phases() are every mark, in order. A span stays valid
/// until the next call to the sink; a reader copies what it keeps.
class TimelineView {
 public:
  TimelineView() = default;
  TimelineView(const TimelineView&) = delete;
  TimelineView(TimelineView&&) = delete;
  TimelineView& operator=(const TimelineView&) = delete;
  TimelineView& operator=(TimelineView&&) = delete;
  virtual ~TimelineView() = default;

  [[nodiscard]] virtual std::size_t series_count() const noexcept = 0;
  [[nodiscard]] virtual const Metric& metric(SeriesId series) const = 0;
  [[nodiscard]] virtual std::span<const Point> points(SeriesId series) const = 0;
  [[nodiscard]] virtual std::span<const PhaseMark> phases() const = 0;
};

/// The timeline is both. Not internally synchronised: one thread writes, and
/// readers do not read while it writes -- the owner (the controller)
/// serialises them.
class Timeline : public TimelineSink, public TimelineView {};

}  // namespace loadforge::telemetry

#endif
