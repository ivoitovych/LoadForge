// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_STATISTICS_SIGNATURE_HPP
#define LOADFORGE_STATISTICS_SIGNATURE_HPP

// CONTRACT (ADR 0004). The Load Signature: what a window of the timeline
// comes to, metric by metric (IMPLEMENTATION M3). Frozen: changed only by a
// contract change, never inside a stream.
//
//   Provider: the statistics stream (src/statistics/).
//   Consumers: results (controller/results/result.hpp), which records the
//     run's signature; explore (explore/trial.hpp), whose objectives read it;
//     reporters, through results.
//
// ABSENCE STAYS ABSENCE
// ---------------------
// A metric with no reading in the window is summarised as Unavailable, with
// the reason and remedy its readings gave -- never as zeros (M3 exit
// criterion 3). An objective built on a metric that is unavailable fails
// loudly (explore/trial.hpp); a summary of zero would let it score silently.
//
// DETERMINISM
// -----------
// summarize() is a pure function of the points in the window: every sum is
// taken in timestamp order with Neumaier compensation, so one timeline gives
// one signature, bit for bit, on every machine.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::statistics {

/// [start_ns, end_ns): a point at start_ns is in, one at end_ns is not.
struct Window {
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
};

[[nodiscard]] inline bool operator==(const Window& lhs, const Window& rhs) noexcept {
  return std::tie(lhs.start_ns, lhs.end_ns) == std::tie(rhs.start_ns, rhs.end_ns);
}

/// A gauge over the window: its present readings only.
///
///   samples      present readings in the window; at least 1.
///   min, max, mean, first, last   of those readings; first and last by time.
///   stddev       the sample standard deviation (n - 1 denominator); absent
///                with one reading.
///   slope_per_second   the least-squares slope of value against time in
///                seconds -- (t_ns - first t_ns) / 1e9 -- dT/dt for a
///                temperature. Absent with one reading.
///                Comparable only where the mode controls the starting
///                state (PLAN §4.4); the result records which.
struct GaugeSummary {
  std::uint64_t samples = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::optional<double> stddev;
  double first = 0.0;
  double last = 0.0;
  std::optional<double> slope_per_second;
};

[[nodiscard]] inline bool operator==(const GaugeSummary& lhs, const GaugeSummary& rhs) noexcept {
  return std::tie(lhs.samples, lhs.min, lhs.max, lhs.mean, lhs.stddev, lhs.first, lhs.last,
                  lhs.slope_per_second) == std::tie(rhs.samples, rhs.min, rhs.max, rhs.mean,
                                                    rhs.stddev, rhs.first, rhs.last,
                                                    rhs.slope_per_second);
}

/// A counter over the window.
///
///   samples    present readings in the window; at least 1.
///   delta      the sum of counter_delta() over consecutive present readings
///              with no Unavailable between them: across a gap the counter
///              may have reset, and an advance that cannot be known is not
///              guessed.
///   rate_per_second   delta / (span_ns / 1e9), span_ns the integer sum of
///              the time those consecutive pairs span; absent when no such
///              pair exists.
struct CounterSummary {
  std::uint64_t samples = 0;
  std::uint64_t delta = 0;
  std::optional<double> rate_per_second;
};

[[nodiscard]] inline bool operator==(const CounterSummary& lhs,
                                     const CounterSummary& rhs) noexcept {
  return std::tie(lhs.samples, lhs.delta, lhs.rate_per_second) ==
         std::tie(rhs.samples, rhs.delta, rhs.rate_per_second);
}

using Summary = std::variant<telemetry::Unavailable, GaugeSummary, CounterSummary>;

/// One metric over the window.
///
///   value                 the summary, or -- when no present reading fell in
///                         the window -- the last Unavailable reading in it;
///                         with none at all, kAbsent with detail "no sample in
///                         the window" and a remedy to sample longer or more
///                         often.
///   unavailable_samples   Unavailable readings in the window: a sensor that
///                         dropped out for part of a run says so beside its
///                         summary.
struct MetricSummary {
  telemetry::Metric metric;
  Summary value;
  std::uint64_t unavailable_samples = 0;
};

/// The window and one summary per series, in the timeline's series order.
struct LoadSignature {
  Window window;
  std::vector<MetricSummary> metrics;
};

/// Why a signature could not be computed.
///
///   kWindow   end_ns is not after start_ns.
enum class StatisticsFault : std::uint8_t { kWindow };

[[nodiscard]] constexpr std::string_view describe(StatisticsFault fault) noexcept {
  switch (fault) {
    case StatisticsFault::kWindow:
      return "empty or inverted window";
  }
  return "unrecognised statistics fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct StatisticsError {
  StatisticsFault fault = StatisticsFault::kWindow;
  std::string detail;
};

/// Computes the signature of a window.
///
/// summarize(view, window) answers one MetricSummary per series of `view`, in
/// series order, by the rules above; kWindow for a window whose end is not
/// after its start. It reads the view and keeps nothing of it.
///
/// Thread-safety: const, and safe to call concurrently on views nobody writes.
class Summarizer {
 public:
  Summarizer() = default;
  Summarizer(const Summarizer&) = delete;
  Summarizer(Summarizer&&) = delete;
  Summarizer& operator=(const Summarizer&) = delete;
  Summarizer& operator=(Summarizer&&) = delete;
  virtual ~Summarizer() = default;

  [[nodiscard]] virtual core::Result<LoadSignature, StatisticsError> summarize(
      const telemetry::TimelineView& view, const Window& window) const = 0;
};

}  // namespace loadforge::statistics

#endif
