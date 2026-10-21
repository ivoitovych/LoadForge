// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_THERMAL_CHANNEL_HPP
#define LOADFORGE_TELEMETRY_SOURCES_THERMAL_CHANNEL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// What the user can do about each way a channel can fail, one sentence per
/// state. kPresent has no remedy and answers with an empty string. Every other
/// state's is non-empty (the contract: an absence without a remedy is a silent
/// one), and each names the provider that failed, not a generic fix (PLAN F3).
class Remedies {
 public:
  Remedies() = default;
  Remedies(std::string absent, std::string vanished, std::string denied, std::string malformed,
           std::string unreadable)
      : absent_(std::move(absent)),
        vanished_(std::move(vanished)),
        denied_(std::move(denied)),
        malformed_(std::move(malformed)),
        unreadable_(std::move(unreadable)) {}

  [[nodiscard]] const std::string& for_availability(topology::Availability kind) const noexcept;

 private:
  std::string absent_;
  std::string vanished_;
  std::string denied_;
  std::string malformed_;
  std::string unreadable_;
  std::string none_;  ///< Always empty: what kPresent, which has no remedy, answers with.
};

/// A Metric, filled in field by field.
///
/// Built this way, and not as `Metric{...}` in the caller, on purpose: an
/// aggregate with two allocating members (the name and the provider) built in
/// one expression carries a cleanup for the half-built object, and the branch
/// inside it is one no test can take (docs/JOURNAL.md 1.13). Here the only
/// allocation that can throw while another is live is an assignment to a
/// complete object.
[[nodiscard]] Metric make_metric(std::string_view name, Unit unit, Kind kind, Scope scope,
                                 std::uint32_t index, std::string_view provider);

/// The words every source's Unavailable is built from: the kind, where, what
/// the kernel or the parser said, and the remedy for that kind.
[[nodiscard]] Unavailable make_unavailable(const topology::Unavailable& failure,
                                           const Remedies& remedies);

/// One sysfs attribute holding a single integer, read as a gauge.
///
/// sysfs hwmon values are integers in a fixed sub-unit (millidegrees, rpm,
/// kHz); the reading is raw * multiplier / divisor, in the metric's unit. A
/// division rather than a multiplication by 0.001, because 20016 / 1000.0 is
/// the double nearest 20.016 and 20016 * 0.001 is 20.016000000000002. A raw
/// value outside [minimum, maximum] is refused as malformed: a sensor that
/// reports 4294967 degrees is not reporting a temperature, and a gauge that
/// repeated it would put a plausible-looking lie in a column (F20).
///
/// The channel remembers whether it has ever been read, through
/// topology::Source, which is what lets ENOENT after a success be kVanished
/// and ENOENT before one be kAbsent.
struct GaugeSpec {
  Metric metric;
  std::string path;
  std::int64_t minimum = 0;
  std::int64_t maximum = 0;
  double multiplier = 1.0;
  double divisor = 1.0;
  Remedies remedies;
};

class Channel {
 public:
  Channel(platform::FileSystem& filesystem, GaugeSpec spec);

  [[nodiscard]] const Metric& metric() const noexcept { return spec_.metric; }

  /// The first read: the metric as the machine offers it.
  [[nodiscard]] MetricStatus probe();

  /// Every later read: a Gauge, or an Unavailable that is never kAbsent.
  [[nodiscard]] Reading read();

 private:
  [[nodiscard]] core::Result<double, topology::Unavailable> value();

  GaugeSpec spec_;
  topology::Source source_;
};

}  // namespace loadforge::telemetry

#endif
