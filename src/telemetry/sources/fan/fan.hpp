// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_FAN_FAN_HPP
#define LOADFORGE_TELEMETRY_SOURCES_FAN_FAN_HPP

#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/thermal/channel.hpp"

namespace loadforge::telemetry {

/// Fan speeds from hwmon (/sys/class/hwmon/hwmonN/fanM_input), in rpm.
///
/// Every input is `fan.speed`, a device-scope gauge whose series index encodes
/// the chip and input numbers (hwmon_series_index), and whose provider is the
/// chip's driver. Which fan an input is -- CPU, case, a pump -- is the board's
/// business and often unlabelled, so nothing here claims to know; the label
/// file is not read. A reading of 0 is a stopped fan and is a reading: absence
/// is a state of the file, never a value (F3).
///
/// A machine with no fan input reports `fan.speed` absent, with the reason.
/// There is no wrapped state: speeds are gauges.
class FanSource final : public Source {
 public:
  explicit FanSource(platform::FileSystem& filesystem, std::string root = {});

  [[nodiscard]] std::string_view name() const noexcept override { return "fan"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<Channel> channels_;
};

}  // namespace loadforge::telemetry

#endif
