// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_THERMAL_CHANNEL_HPP
#define LOADFORGE_TELEMETRY_SOURCES_THERMAL_CHANNEL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// What the user can do about each way a channel can fail, one sentence per
/// state, indexed by topology::Availability. kPresent has no remedy and its
/// slot stays empty. Every other slot is non-empty (the contract: an absence
/// without a remedy is a silent one), and each names the provider that
/// failed, not a generic fix (PLAN F3).
struct Remedies {
  std::array<std::string, 6> by_availability;

  [[nodiscard]] const std::string& for_availability(topology::Availability kind) const noexcept {
    return by_availability[static_cast<std::size_t>(kind)];
  }
};

[[nodiscard]] Remedies make_remedies(std::string absent, std::string vanished, std::string denied,
                                     std::string malformed, std::string unreadable);

/// The words every source's Unavailable is built from: the kind, where, what
/// the kernel or the parser said, and the remedy for that kind.
[[nodiscard]] Unavailable make_unavailable(const topology::Unavailable& failure,
                                           const Remedies& remedies);

/// One sysfs attribute holding a single integer, read as a gauge.
///
/// sysfs hwmon values are integers in a fixed sub-unit (millidegrees, rpm,
/// kHz); the reading is raw * multiplier / divisor, in the metric's unit (a
/// division rather than a multiplication by 0.001, because 43700 / 1000.0 is
/// the double nearest 43.7 and 43700 * 0.001 is not). A raw value
/// outside [minimum, maximum] is refused as malformed: a sensor that reports
/// -128000 or 4294967 degrees is not reporting a temperature, and a gauge that
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
