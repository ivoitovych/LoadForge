// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SAMPLE_HPP
#define LOADFORGE_TELEMETRY_SAMPLE_HPP

// CONTRACT (ADR 0004). The vocabulary of telemetry: what a metric is, what a
// reading of one is, and what "no reading" means. Frozen: changed only by a
// contract change, never inside a stream.
//
//   Producers: every telemetry source (source.hpp).
//   Consumers: the sampler and the timeline (timeline.hpp), statistics,
//     results and every reporter, the crash journal, explore's objectives.
//
// EVERY METRIC IS NULLABLE, AND ABSENCE CARRIES A REMEDY
// ------------------------------------------------------
// PLAN F3: availability varies by source, kernel, platform and privilege. A
// reading is a value or an Unavailable, never a zero standing in for one, and
// an Unavailable says which of topology's six kinds of could-not it is, where,
// why, and what the user can do about it -- the remedy for the provider that
// actually failed (F3's table), or that nothing can be done. Advising a fix
// that cannot work is worse than saying nothing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include "topology/source.hpp"

namespace loadforge::telemetry {

enum class Unit : std::uint8_t {
  kCelsius,
  kWatts,
  kJoules,
  kHertz,
  kRpm,
  kBytes,
  kRatio,  ///< 0 to 1: utilisation, a share.
  kCount,  ///< events: corrected errors, context switches.
  kVolts,
  /// An energy counter's own unit: the raw value of RAPL's energy_uj and of the
  /// power PMU once scaled. A counter is an integer, so joules would lose the
  /// fraction; the metric names the unit its raw value is in.
  kMicrojoules,
};

[[nodiscard]] constexpr std::string_view describe(Unit unit) noexcept {
  switch (unit) {
    case Unit::kCelsius:
      return "celsius";
    case Unit::kWatts:
      return "watts";
    case Unit::kJoules:
      return "joules";
    case Unit::kHertz:
      return "hertz";
    case Unit::kRpm:
      return "rpm";
    case Unit::kBytes:
      return "bytes";
    case Unit::kRatio:
      return "ratio";
    case Unit::kCount:
      return "count";
    case Unit::kVolts:
      return "volts";
    case Unit::kMicrojoules:
      return "microjoules";
  }
  return "unrecognised unit";
}

/// A gauge is read as it is (a temperature). A counter is a raw value that only
/// ever increases between resets, modulo its wrap if it has one, and whose
/// differences mean something (energy in microjoules, corrected-error counts).
///
/// A counter's raw value is in its metric's unit, exactly: nothing downstream
/// scales it, so a counter of energy is in kMicrojoules and never in kJoules.
/// An ESTIMATE is not a counter, even when it is an integer: a count scaled up
/// for the time an event was not on the PMU goes down when the next, exact,
/// reading arrives, and a counter that goes down is read as a wrap or a reset.
/// A source reports the exact raw count as the counter, and how much of the time
/// the count covers as a separate gauge (the counters source's `pmu_share`).
enum class Kind : std::uint8_t { kGauge, kCounter };

/// What a metric is attributed to; the index says which one (package 0, CPU
/// 11, memory controller 1). kMachine has index 0.
enum class Scope : std::uint8_t {
  kMachine,
  kPackage,
  kCore,
  kCpu,
  kNumaNode,
  kMemoryController,
  kDimm,
  kDevice,
};

[[nodiscard]] constexpr std::string_view describe(Scope scope) noexcept {
  switch (scope) {
    case Scope::kMachine:
      return "machine";
    case Scope::kPackage:
      return "package";
    case Scope::kCore:
      return "core";
    case Scope::kCpu:
      return "cpu";
    case Scope::kNumaNode:
      return "node";
    case Scope::kMemoryController:
      return "memory controller";
    case Scope::kDimm:
      return "dimm";
    case Scope::kDevice:
      return "device";
  }
  return "unrecognised scope";
}

/// One metric.
///
///   name         "<family>.<quantity>", lower-case letters, digits and '_'
///                in each part: "thermal.package", "power.package",
///                "frequency.cpu", "edac.corrected". The family is the
///                source's name.
///   unit, kind   as above.
///   scope, scope_index   what it is attributed to. (name, scope,
///                scope_index) identifies a series; nothing else does.
///   modulus      for a counter that can wrap within a run, the value it wraps
///                at: readings are in [0, modulus). Absent for a gauge, and for
///                a counter that does not wrap within a run -- a 64-bit one
///                (centuries at any rate), or a 32-bit one whose rate cannot
///                reach 2^32 in a run (the memory controllers' error counts).
///                A step down in a counter with no modulus is not a wrap: it is
///                a RESET (the hardware's counters cleared, a driver reloaded,
///                an event reopened), and the advance across it is unknown --
///                counter_advance() says so, and the summarizer treats it as a
///                gap and counts it, as it does an Unavailable reading.
///   provider     the mechanism behind it, for the remedy and the report:
///                "hwmon:coretemp", "powercap:intel-rapl", "perf:power".
struct Metric {
  std::string name;
  Unit unit = Unit::kCount;
  Kind kind = Kind::kGauge;
  Scope scope = Scope::kMachine;
  std::uint32_t scope_index = 0;
  std::optional<std::uint64_t> modulus;
  std::string provider;
};

[[nodiscard]] inline bool operator==(const Metric& lhs, const Metric& rhs) noexcept {
  return std::tie(lhs.name, lhs.unit, lhs.kind, lhs.scope, lhs.scope_index, lhs.modulus,
                  lhs.provider) == std::tie(rhs.name, rhs.unit, rhs.kind, rhs.scope,
                                            rhs.scope_index, rhs.modulus, rhs.provider);
}

/// Whether two metrics are the same series: same name, scope and index.
[[nodiscard]] inline bool same_series(const Metric& lhs, const Metric& rhs) noexcept {
  return std::tie(lhs.name, lhs.scope, lhs.scope_index) ==
         std::tie(rhs.name, rhs.scope, rhs.scope_index);
}

/// Whether a name keeps the rule above: two or more non-empty dot-separated
/// parts, each of lower-case letters, digits and '_', each starting with a
/// letter.
[[nodiscard]] constexpr bool is_valid_metric_name(std::string_view name) noexcept {
  int parts = 0;
  bool at_start = true;
  for (const char c : name) {
    if (c == '.') {
      if (at_start) {
        return false;
      }
      at_start = true;
      continue;
    }
    const bool letter = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (at_start && !letter) {
      return false;
    }
    if (!letter && !digit && c != '_') {
      return false;
    }
    if (at_start) {
      ++parts;
      at_start = false;
    }
  }
  return parts >= 2 && !at_start;
}

/// Why there is no value. `kind` is never kPresent. `remediation` is never
/// empty: what the user can do, for the provider that failed, or a plain
/// statement that this platform does not offer it and nothing will change
/// that.
struct Unavailable {
  topology::Availability kind = topology::Availability::kUnreadable;
  std::string path;
  std::string detail;
  std::string remediation;
};

[[nodiscard]] inline bool operator==(const Unavailable& lhs, const Unavailable& rhs) noexcept {
  return std::tie(lhs.kind, lhs.path, lhs.detail, lhs.remediation) ==
         std::tie(rhs.kind, rhs.path, rhs.detail, rhs.remediation);
}

/// A gauge's value: finite. A source that reads a non-finite one reports
/// kMalformed instead.
struct Gauge {
  double value = 0.0;
};

[[nodiscard]] inline bool operator==(Gauge lhs, Gauge rhs) noexcept {
  return lhs.value == rhs.value;
}

/// A counter's raw value, below the metric's modulus when it has one.
struct Counter {
  std::uint64_t raw = 0;
};

[[nodiscard]] inline bool operator==(Counter lhs, Counter rhs) noexcept {
  return lhs.raw == rhs.raw;
}

using Reading = std::variant<Unavailable, Gauge, Counter>;

/// Whether two readings are the same: the same alternative, equal within it.
/// Written out rather than std::variant's ==, which the analysers cannot see
/// is noexcept.
[[nodiscard]] inline bool same_reading(const Reading& lhs, const Reading& rhs) noexcept {
  if (lhs.index() != rhs.index()) {
    return false;
  }
  if (const auto* gauge = std::get_if<Gauge>(&lhs)) {
    return *gauge == *std::get_if<Gauge>(&rhs);
  }
  if (const auto* counter = std::get_if<Counter>(&lhs)) {
    return *counter == *std::get_if<Counter>(&rhs);
  }
  return *std::get_if<Unavailable>(&lhs) == *std::get_if<Unavailable>(&rhs);
}

/// How far a counter advanced from `previous` to `current`, across at most one
/// wrap: (current - previous) modulo `modulus`, or modulo 2^64 without one.
/// Exactly one wrap between two readings is indistinguishable from none plus
/// the difference, which is why the sampler reads a wrapping counter more
/// often than it can wrap (sampler's contract, timeline.hpp).
[[nodiscard]] constexpr std::uint64_t counter_delta(std::uint64_t previous, std::uint64_t current,
                                                    std::optional<std::uint64_t> modulus) noexcept {
  if (!modulus.has_value() || current >= previous) {
    return current - previous;
  }
  return *modulus - previous + current;
}

/// How far a counter advanced from `previous` to `current` by the rules of the
/// metric's modulus, or nothing when that cannot be known.
///
/// With a modulus a step down is a wrap, and the advance is counter_delta().
/// Without one the counter does not wrap within a run, so a step down is a reset
/// and the advance across it is unknown: absent, never 2^64 - n. Statistics reads
/// every pair of consecutive readings through this function.
[[nodiscard]] constexpr std::optional<std::uint64_t> counter_advance(
    std::uint64_t previous, std::uint64_t current, std::optional<std::uint64_t> modulus) noexcept {
  if (!modulus.has_value() && current < previous) {
    return std::nullopt;
  }
  return counter_delta(previous, current, modulus);
}

}  // namespace loadforge::telemetry

#endif
