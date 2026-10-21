// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_THERMAL_THERMAL_HPP
#define LOADFORGE_TELEMETRY_SOURCES_THERMAL_THERMAL_HPP

#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/thermal/channel.hpp"

namespace loadforge::telemetry {

/// Temperatures from hwmon (/sys/class/hwmon/hwmonN/tempM_input) and from the
/// thermal zones (/sys/class/thermal/thermal_zoneN/temp), in degrees Celsius.
///
/// PLAN F3: for hwmon the hard problem is identification, not permission. A
/// sensor is named by what the driver says it is -- coretemp's "Package id N"
/// and "Core N", k10temp's "Tdie" -- and every other sensor is reported as
/// `thermal.sensor` with the chip that owns it as its provider and a series
/// index that encodes the chip and input numbers, so nothing is guessed to be
/// the package temperature and nothing is hidden for not being recognised.
/// Thermal zones are `thermal.zone`, one per zone, provider
/// "thermal_zone:<type>": the kernel's own view, which on some machines
/// duplicates an hwmon chip and is kept distinct from it rather than merged.
///
/// A machine with neither reports `thermal.package` absent, with the reason.
///
/// There is no wrapped state: temperatures are gauges.
class ThermalSource final : public Source {
 public:
  /// `root` prefixes every path ("" for the machine; a test tree's directory
  /// otherwise).
  explicit ThermalSource(platform::FileSystem& filesystem, std::string root = {});

  [[nodiscard]] std::string_view name() const noexcept override { return "thermal"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<Channel> channels_;  ///< The ones present at probe(), in probe order.
};

}  // namespace loadforge::telemetry

#endif
