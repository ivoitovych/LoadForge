// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCE_HPP
#define LOADFORGE_TELEMETRY_SOURCE_HPP

// CONTRACT (ADR 0004). A telemetry source, and the capability report built
// from all of them (PLAN F3, IMPLEMENTATION M2a and M3). Frozen: changed only
// by a contract change, never inside a stream.
//
//   Providers: the telemetry-source streams, one directory per source under
//     src/telemetry/sources/<name>/ (thermal, fan, frequency, memory, power,
//     counters, edac), each reading through the platform seam so that every
//     state can be driven from fixtures.
//   Consumers: the sampler (timeline.hpp), and the capability probe that
//     reports the set at the start of every run.
//
// THE SIX STATES, PER SOURCE
// --------------------------
// IMPLEMENTATION M3: every source owes six fixtures -- present, absent,
// permission-denied, malformed, disappears mid-run, counter-wraps -- and each
// arrives through this interface as something specific: a Reading, an
// Unavailable of the matching kind at probe(), an Unavailable of kVanished at
// sample(), or a counter whose raw value went down (counter_delta in
// sample.hpp says what that means).

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "telemetry/sample.hpp"

namespace loadforge::telemetry {

/// One metric as probe() found it: present (no `unavailable`), or why not.
struct MetricStatus {
  Metric metric;
  std::optional<Unavailable> unavailable;
};

[[nodiscard]] inline bool operator==(const MetricStatus& lhs, const MetricStatus& rhs) noexcept {
  return std::tie(lhs.metric, lhs.unavailable) == std::tie(rhs.metric, rhs.unavailable);
}

/// One source of metrics.
///
/// name() is the source's family: the first part of every metric name it
/// reports ("thermal" for "thermal.package").
///
/// probe() discovers what this machine offers and fixes the source's metric
/// set; it is called once, before any sample(). It returns at least one
/// status -- a source that finds nothing reports the metrics it looked for
/// as unavailable, never an empty list, because a silently empty column is the
/// worst outcome F3 names. Every metric's name is valid and starts with
/// name() and '.'; no two statuses are the same series; every Unavailable has
/// a kind other than kPresent and a non-empty remediation.
///
/// sample(out) reads every metric probe() found present, once, and appends
/// exactly one Reading per present metric to `out`, in probe() order. A
/// reading is a Gauge for a gauge metric and a Counter for a counter metric,
/// or an Unavailable -- kVanished for one that was present and is gone,
/// kDenied, kMalformed or kUnreadable otherwise; never kAbsent, because a
/// metric that was there at probe() was not absent. A metric that comes back
/// after vanishing is read again. A Gauge is finite; a Counter with a modulus
/// is below it. Before probe(), sample() appends nothing. sample() does not
/// sleep or wait: its time is the reads it makes (F17 -- the observer's cost
/// is measured, so it must be bounded).
///
/// Thread-safety: none. A source belongs to the thread that samples it.
class Source {
 public:
  Source() = default;
  Source(const Source&) = delete;
  Source(Source&&) = delete;
  Source& operator=(const Source&) = delete;
  Source& operator=(Source&&) = delete;
  virtual ~Source() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::vector<MetricStatus> probe() = 0;
  virtual void sample(std::vector<Reading>& out) = 0;
};

/// One source's probe, for the report.
struct SourceStatus {
  std::string source;
  std::vector<MetricStatus> metrics;
};

[[nodiscard]] inline bool operator==(const SourceStatus& lhs, const SourceStatus& rhs) noexcept {
  return std::tie(lhs.source, lhs.metrics) == std::tie(rhs.source, rhs.metrics);
}

/// The capability set, reported at the start of every run (M2a exit
/// criterion 5) and recorded in the result: every source, every metric, and
/// for every metric that is not present, why and what to do.
struct CapabilityReport {
  std::vector<SourceStatus> sources;
};

[[nodiscard]] inline bool operator==(const CapabilityReport& lhs,
                                     const CapabilityReport& rhs) noexcept {
  return lhs.sources == rhs.sources;
}

/// How many metrics the report holds, and how many of them are present.
struct CapabilityCounts {
  std::size_t metrics = 0;
  std::size_t present = 0;
};

[[nodiscard]] inline CapabilityCounts count(const CapabilityReport& report) noexcept {
  CapabilityCounts counts;
  for (const SourceStatus& source : report.sources) {
    for (const MetricStatus& status : source.metrics) {
      ++counts.metrics;
      if (!status.unavailable.has_value()) {
        ++counts.present;
      }
    }
  }
  return counts;
}

}  // namespace loadforge::telemetry

#endif
