// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/memory/memory.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "resources/decimal.hpp"
#include "telemetry/sources/thermal/channel.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

struct Field {
  std::string_view key;     ///< "MemAvailable"
  std::string_view metric;  ///< "memory.available"
};

constexpr std::array<Field, 7> kFields{{
    {"MemTotal", "memory.total"},
    {"MemFree", "memory.free"},
    {"MemAvailable", "memory.available"},
    {"Buffers", "memory.buffers"},
    {"Cached", "memory.cached"},
    {"SwapTotal", "memory.swap_total"},
    {"SwapFree", "memory.swap_free"},
}};

constexpr std::uint64_t kBytesPerKilobyte = 1024;
constexpr std::string_view kKilobytes = " kB";

Remedies field_remedies(const Field& field) {
  const std::string key{field.key};
  return make_remedies(
      "this system does not offer " + key +
          " in /proc/meminfo: procfs may not be mounted (some containers and chroots), or the "
          "kernel predates the line (MemAvailable arrived in Linux 3.14; the swap lines are "
          "absent when swap support is compiled out); nothing LoadForge can change, and it "
          "does not substitute an estimate",
      "the " + key +
          " figure disappeared from /proc/meminfo during the run (procfs unmounted or the "
          "line gone); re-run once the system is stable",
      "read access to /proc/meminfo is refused, which is unusual (it is world-readable by "
      "default); a sandbox or security-module policy may be in force, so check it",
      "/proc/meminfo does not hold \"" + key +
          ": <number> kB\" where it should, and LoadForge does not guess; this is a kernel or "
          "sandbox fault, so report the kernel version",
      "the read of /proc/meminfo failed; check the kernel log (dmesg)");
}

Metric memory_metric(const Field& field) {
  return Metric{std::string{field.metric}, Unit::kBytes, Kind::kGauge, Scope::kMachine,
                0,                         std::nullopt, "procfs:meminfo"};
}

/// The field's bytes; nothing when there is no such line; an error when the
/// line (or a second one for the same key) is not one the kernel writes.
core::Result<std::optional<double>, std::string> find_field(
    const std::vector<std::string>& lines, const Field& field) {
  const std::string prefix = std::string{field.key} + ":";
  std::optional<double> found;
  for (const std::string& line : lines) {
    std::string_view rest{line};
    if (!rest.starts_with(prefix)) {
      continue;
    }
    if (found.has_value()) {
      return std::string{field.key} + " appears twice";
    }
    rest.remove_prefix(prefix.size());
    while (rest.starts_with(' ')) {
      rest.remove_prefix(1);
    }
    if (!rest.ends_with(kKilobytes)) {
      return "\"" + line + "\" is not in kB";
    }
    rest.remove_suffix(kKilobytes.size());
    const std::optional<std::uint64_t> kilobytes = resources::parse_decimal(rest);
    if (!kilobytes.has_value() ||
        *kilobytes > std::numeric_limits<std::uint64_t>::max() / kBytesPerKilobyte) {
      return "\"" + line + "\" does not hold a number of kB a byte count can carry";
    }
    found = static_cast<double>(*kilobytes * kBytesPerKilobyte);
  }
  return found;
}

/// The field in this read of the file, or why not. `missing` is what a line
/// that is not there means: absent at the probe, vanished once it has been seen.
core::Result<double, topology::Unavailable> field_value(
    const core::Result<std::vector<std::string>, topology::Unavailable>& lines,
    const std::string& path, const Field& field, topology::Availability missing) {
  if (!lines.has_value()) {
    return lines.error();
  }
  auto found = find_field(lines.value(), field);
  if (!found.has_value()) {
    return topology::Unavailable{topology::Availability::kMalformed, path, found.error()};
  }
  if (!found.value().has_value()) {
    return topology::Unavailable{missing, path, "no " + std::string{field.key} + " line"};
  }
  return *found.value();
}

}  // namespace

MemorySource::MemorySource(platform::FileSystem& filesystem, std::string root)
    : meminfo_(filesystem, root + "/proc/meminfo") {}

std::vector<MetricStatus> MemorySource::probe() {
  present_.clear();
  std::vector<MetricStatus> statuses;
  const auto lines = meminfo_.lines();
  for (std::size_t at = 0; at < kFields.size(); ++at) {
    const Field& field = kFields[at];
    std::optional<Unavailable> unavailable;
    const auto value = field_value(lines, meminfo_.path(), field, topology::Availability::kAbsent);
    if (value.has_value()) {
      present_.push_back(at);
    } else {
      unavailable = make_unavailable(value.error(), field_remedies(field));
    }
    statuses.push_back(MetricStatus{memory_metric(field), std::move(unavailable)});
  }
  return statuses;
}

void MemorySource::sample(std::vector<Reading>& out) {
  if (present_.empty()) {
    return;
  }
  const auto lines = meminfo_.lines();
  for (const std::size_t at : present_) {
    const Field& field = kFields[at];
    const auto value =
        field_value(lines, meminfo_.path(), field, topology::Availability::kVanished);
    if (value.has_value()) {
      out.emplace_back(Gauge{value.value()});
    } else {
      out.emplace_back(make_unavailable(value.error(), field_remedies(field)));
    }
  }
}

}  // namespace loadforge::telemetry
