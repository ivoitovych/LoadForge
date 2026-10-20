// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/edac/edac.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/parse_number.hpp"
#include "core/result.hpp"
#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

constexpr std::string_view kProvider = "edac:sysfs";
constexpr std::string_view kCorrected = "edac.corrected";
constexpr std::string_view kUncorrected = "edac.uncorrected";

/// One count, as the kernel writes it: a decimal number below 2^32, nothing
/// else on the line. Anything else is refused, never guessed at (F20).
core::Result<std::uint64_t, topology::Unavailable> read_count(topology::Source& file) {
  auto text = file.text();
  if (!text.has_value()) {
    return text.error();
  }
  auto parsed = core::split_number_and_suffix(text.value());
  if (!parsed.has_value() || !parsed.value().suffix.empty()) {
    std::string detail = "not a decimal count: '";
    detail += text.value();
    detail += "'";
    return topology::Unavailable{topology::Availability::kMalformed, file.path(),
                                 std::move(detail)};
  }
  if (parsed.value().value >= EdacSource::kModulus) {
    std::string detail = "exceeds the 32-bit count the kernel keeps: ";
    detail += text.value();
    return topology::Unavailable{topology::Availability::kMalformed, file.path(),
                                 std::move(detail)};
  }
  return parsed.value().value;
}

/// The report's form of a failed read: the same words, plus what to do.
Unavailable to_unavailable(topology::Unavailable failure) {
  std::string remediation = edac_remediation(failure.kind, failure.path);
  return Unavailable{failure.kind, std::move(failure.path), std::move(failure.detail),
                     std::move(remediation)};
}

Metric make_metric(std::string_view name, Scope scope, std::uint32_t index) {
  Metric metric;
  metric.name = std::string{name};
  metric.unit = Unit::kCount;
  metric.kind = Kind::kCounter;
  metric.scope = scope;
  metric.scope_index = index;
  metric.modulus = EdacSource::kModulus;
  metric.provider = std::string{kProvider};
  return metric;
}

}  // namespace

std::string edac_remediation(topology::Availability kind, std::string_view path) {
  std::string text;
  switch (kind) {
    case topology::Availability::kAbsent:
      return "this platform does not report memory errors through EDAC (no driver for its "
             "memory controller, as on most consumer boards); nothing can be changed from "
             "the software side";
    case topology::Availability::kVanished:
      text = "the memory controller went away during the run (driver unloaded or memory "
             "removed); read the kernel log (dmesg), and treat counts gathered before it as "
             "from a machine that has since changed";
      return text;
    case topology::Availability::kDenied:
      text = "EDAC counts are normally world-readable, so reading ";
      text += path;
      text += " was refused by a hardened sysfs mount or a security policy (SELinux, "
              "AppArmor); allow read access to /sys/devices/system/edac, or run with it";
      return text;
    case topology::Availability::kMalformed:
      text = "the kernel writes a decimal count to ";
      text += path;
      text += " and it held something else; report its content and the kernel version";
      return text;
    case topology::Availability::kUnreadable:
    case topology::Availability::kPresent:
      break;
  }
  text = "reading ";
  text += path;
  text += " failed; read the kernel log (dmesg) for memory-controller driver errors";
  return text;
}

EdacSource::EdacSource(platform::FileSystem& filesystem, std::string_view sysfs_root)
    : filesystem_(&filesystem), root_(sysfs_root) {}

std::vector<MetricStatus> EdacSource::probe() {
  channels_.clear();
  std::vector<MetricStatus> statuses;

  // One attribute of one controller. Returns false, appending nothing, when
  // `stop_if_absent` and the attribute does not exist: the controller list has
  // ended. Anything else is a status -- present, or why not.
  const auto probe_attribute = [&](const std::string& directory, std::string_view attribute,
                                   std::string_view metric_name, std::uint32_t index,
                                   bool stop_if_absent) {
    std::string path = directory;
    path += "/";
    path += attribute;
    topology::Source file{*filesystem_, path};
    auto count = read_count(file);
    if (count.has_value()) {
      channels_.push_back(Channel{file});
      statuses.push_back(
          MetricStatus{make_metric(metric_name, Scope::kMemoryController, index), std::nullopt});
      return true;
    }
    if (stop_if_absent && count.error().kind == topology::Availability::kAbsent) {
      return false;
    }
    statuses.push_back(MetricStatus{make_metric(metric_name, Scope::kMemoryController, index),
                                    to_unavailable(count.error())});
    return true;
  };

  for (std::uint32_t index = 0;; ++index) {
    std::string directory = root_;
    directory += "/devices/system/edac/mc/mc";
    directory += std::to_string(index);
    if (!probe_attribute(directory, "ce_count", kCorrected, index, true)) {
      break;
    }
    probe_attribute(directory, "ue_count", kUncorrected, index, false);
  }

  if (statuses.empty()) {
    std::string path = root_;
    path += "/devices/system/edac/mc";
    for (const std::string_view name : {kCorrected, kUncorrected}) {
      Unavailable absent{topology::Availability::kAbsent, path,
                         "no memory controller is registered with EDAC",
                         edac_remediation(topology::Availability::kAbsent, path)};
      statuses.push_back(MetricStatus{make_metric(name, Scope::kMachine, 0), std::move(absent)});
    }
  }
  return statuses;
}

void EdacSource::sample(std::vector<Reading>& out) {
  for (Channel& channel : channels_) {
    auto count = read_count(channel.file);
    if (count.has_value()) {
      out.emplace_back(Counter{count.value()});
    } else {
      out.emplace_back(to_unavailable(count.error()));
    }
  }
}

}  // namespace loadforge::telemetry
