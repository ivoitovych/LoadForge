// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/fan/fan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "telemetry/sources/thermal/hwmon.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

// No fan turns faster than a million rpm; a count above it is a bad read.
constexpr std::int64_t kMaxRpm = 1'000'000;

Remedies fan_remedies(const std::string& provider) {
  return make_remedies(
      "this machine does not offer this fan input: " + provider +
          " publishes no such file; nothing LoadForge can change here",
      "the fan input behind " + provider +
          " disappeared during the run (driver unloaded or device removed); re-run once the "
          "hardware is stable",
      "read access to this " + provider +
          " attribute is refused; fan inputs are normally world-readable, so check the file's "
          "mode, owner and any udev rule rather than running privileged",
      "the " + provider +
          " driver returned something that is not a fan speed, and LoadForge does not guess; "
          "update the kernel or driver, or report the board",
      "the read of this " + provider +
          " attribute failed; check the kernel log (dmesg) for the driver's complaint");
}

GaugeSpec fan_spec(const HwmonChip& chip, std::uint32_t input, std::string path) {
  GaugeSpec spec;
  spec.metric = Metric{"fan.speed",
                       Unit::kRpm,
                       Kind::kGauge,
                       Scope::kDevice,
                       hwmon_series_index(chip, input),
                       std::nullopt,
                       hwmon_provider(chip)};
  spec.remedies = fan_remedies(spec.metric.provider);
  spec.path = std::move(path);
  spec.minimum = 0;
  spec.maximum = kMaxRpm;
  spec.scale = 1.0;
  return spec;
}

}  // namespace

FanSource::FanSource(platform::FileSystem& filesystem, std::string root)
    : filesystem_(&filesystem), root_(std::move(root)) {}

std::vector<MetricStatus> FanSource::probe() {
  channels_.clear();
  std::vector<MetricStatus> statuses;
  for (const HwmonChip& chip : find_hwmon_chips(*filesystem_, root_)) {
    for (std::uint32_t input = 1; input <= kMaxHwmonChannels; ++input) {
      Channel channel(*filesystem_,
                      fan_spec(chip, input, chip.directory + "/fan" + std::to_string(input) +
                                                "_input"));
      MetricStatus status = channel.probe();
      if (status.unavailable.has_value() &&
          status.unavailable->kind == topology::Availability::kAbsent) {
        continue;
      }
      if (!status.unavailable.has_value()) {
        channels_.push_back(std::move(channel));
      }
      statuses.push_back(std::move(status));
    }
  }

  if (statuses.empty()) {
    Unavailable none;
    none.kind = topology::Availability::kAbsent;
    none.path = root_ + "/sys/class/hwmon";
    none.detail = "no hwmon chip publishes a fan input";
    none.remediation =
        "this machine exposes no fan speed to read; laptops, virtual machines, containers and "
        "most single-board computers have none, and on a desktop the Super I/O driver for the "
        "board (nct6775, it87) may not be loaded";
    statuses.push_back(MetricStatus{
        Metric{"fan.speed", Unit::kRpm, Kind::kGauge, Scope::kDevice, 0, std::nullopt, "hwmon"},
        std::move(none)});
  }
  return statuses;
}

void FanSource::sample(std::vector<Reading>& out) {
  for (Channel& channel : channels_) {
    out.push_back(channel.read());
  }
}

}  // namespace loadforge::telemetry
