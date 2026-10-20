// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/thermal/thermal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "resources/decimal.hpp"
#include "telemetry/sources/thermal/hwmon.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

// Millidegrees Celsius. Absolute zero is the floor: nothing below it is a
// temperature. A thousand degrees is the ceiling: the fixed-width garbage a
// failing sensor returns (4294967295, -128000) lands outside one or the other.
constexpr std::int64_t kMinMilliCelsius = -273'150;
constexpr std::int64_t kMaxMilliCelsius = 1'000'000;
constexpr double kCelsiusPerMilli = 0.001;

Remedies thermal_remedies(const std::string& provider) {
  return make_remedies(
      "this machine does not offer this sensor: " + provider +
          " publishes no such input; nothing LoadForge can change here",
      "the sensor behind " + provider +
          " disappeared during the run (driver unloaded, device removed or CPU taken offline); "
          "readings before and after belong to different hardware, so re-run once it is stable",
      "read access to this " + provider +
          " attribute is refused; hwmon and thermal-zone temperatures are normally "
          "world-readable, so check the file's mode, owner and any udev rule rather than "
          "running privileged",
      "the " + provider +
          " driver returned something that is not a temperature, and LoadForge does not guess; "
          "update the kernel or driver, or report the board",
      "the read of this " + provider +
          " attribute failed; a sensor that cannot be read usually means a driver or device in "
          "trouble, so check the kernel log (dmesg)");
}

GaugeSpec gauge_spec(Metric metric, std::string path) {
  GaugeSpec spec;
  spec.remedies = thermal_remedies(metric.provider);
  spec.metric = std::move(metric);
  spec.path = std::move(path);
  spec.minimum = kMinMilliCelsius;
  spec.maximum = kMaxMilliCelsius;
  spec.scale = kCelsiusPerMilli;
  return spec;
}

Metric make_metric(std::string name, Scope scope, std::uint32_t index, std::string provider) {
  return Metric{std::move(name), Unit::kCelsius, Kind::kGauge, scope, index, std::nullopt,
                std::move(provider)};
}

/// The number after `prefix` in `label`, when the label is exactly that.
std::optional<std::uint32_t> numbered(std::string_view label, std::string_view prefix) {
  if (!label.starts_with(prefix)) {
    return std::nullopt;
  }
  label.remove_prefix(prefix.size());
  const std::optional<std::uint64_t> number = resources::parse_decimal(label);
  if (!number.has_value() || *number > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*number);
}

Metric unidentified(const HwmonChip& chip, std::uint32_t channel) {
  return make_metric("thermal.sensor", Scope::kDevice, hwmon_series_index(chip, channel),
                     hwmon_provider(chip));
}

/// What a chip's driver says an input is, when this knows the driver.
Metric identify(const HwmonChip& chip, std::string_view label, std::uint32_t channel) {
  if (chip.name == "coretemp") {
    if (const auto package = numbered(label, "Package id ")) {
      return make_metric("thermal.package", Scope::kPackage, *package, hwmon_provider(chip));
    }
    if (const auto core = numbered(label, "Core ")) {
      return make_metric("thermal.core", Scope::kCore, *core, hwmon_provider(chip));
    }
  } else if (chip.name == "k10temp" && label == "Tdie") {
    return make_metric("thermal.package", Scope::kPackage, 0, hwmon_provider(chip));
  }
  return unidentified(chip, channel);
}

bool is_absent(const MetricStatus& status) {
  return status.unavailable.has_value() &&
         status.unavailable->kind == topology::Availability::kAbsent;
}

bool is_taken(const std::vector<MetricStatus>& statuses, const Metric& metric) {
  return std::any_of(statuses.begin(), statuses.end(), [&metric](const MetricStatus& status) {
    return same_series(status.metric, metric);
  });
}

constexpr std::uint32_t kMaxThermalZones = 64;

}  // namespace

ThermalSource::ThermalSource(platform::FileSystem& filesystem, std::string root)
    : filesystem_(&filesystem), root_(std::move(root)) {}

std::vector<MetricStatus> ThermalSource::probe() {
  channels_.clear();
  std::vector<MetricStatus> statuses;
  const auto record = [this, &statuses](MetricStatus status, Channel channel) {
    if (!status.unavailable.has_value()) {
      channels_.push_back(std::move(channel));
    }
    statuses.push_back(std::move(status));
  };

  for (const HwmonChip& chip : find_hwmon_chips(*filesystem_, root_)) {
    for (std::uint32_t input = 1; input <= kMaxHwmonChannels; ++input) {
      const std::string prefix = chip.directory + "/temp" + std::to_string(input);
      const std::string label = read_label(*filesystem_, prefix + "_label");
      const Metric identified = identify(chip, label, input);
      Channel channel(*filesystem_, gauge_spec(identified, prefix + "_input"));
      MetricStatus status = channel.probe();
      if (is_absent(status)) {
        continue;  // This chip has no such input; that is not a finding.
      }
      if (is_taken(statuses, identified)) {
        // Two inputs claiming one series (two chips both called a package):
        // the second is reported as what it is, not dropped and not merged.
        channel = Channel(*filesystem_, gauge_spec(unidentified(chip, input), prefix + "_input"));
        status = channel.probe();
      }
      record(std::move(status), std::move(channel));
    }
  }

  for (std::uint32_t zone = 0; zone < kMaxThermalZones; ++zone) {
    const std::string directory = root_ + "/sys/class/thermal/thermal_zone" + std::to_string(zone);
    const std::string type = read_label(*filesystem_, directory + "/type");
    std::string provider = "thermal_zone";
    if (!type.empty()) {
      provider += ":" + type;
    }
    Channel channel(*filesystem_,
                    gauge_spec(make_metric("thermal.zone", Scope::kDevice, zone, provider),
                               directory + "/temp"));
    MetricStatus status = channel.probe();
    if (is_absent(status)) {
      continue;
    }
    record(std::move(status), std::move(channel));
  }

  if (statuses.empty()) {
    Unavailable none;
    none.kind = topology::Availability::kAbsent;
    none.path = root_ + "/sys/class/hwmon";
    none.detail = "no hwmon chip with a temperature input and no thermal zone";
    none.remediation =
        "this machine exposes no temperature sensor to read; in a virtual machine or a "
        "container that is the platform's choice and nothing inside it can change that, while "
        "on bare metal the platform's hwmon driver (coretemp on Intel, k10temp on AMD) may not "
        "be loaded";
    statuses.push_back(MetricStatus{
        make_metric("thermal.package", Scope::kPackage, 0, "hwmon"), std::move(none)});
  }
  return statuses;
}

void ThermalSource::sample(std::vector<Reading>& out) {
  for (Channel& channel : channels_) {
    out.push_back(channel.read());
  }
}

}  // namespace loadforge::telemetry
