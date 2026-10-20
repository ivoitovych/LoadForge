// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/thermal/channel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

const std::string& Remedies::for_availability(topology::Availability kind) const noexcept {
  switch (kind) {
    case topology::Availability::kAbsent:
      return absent_;
    case topology::Availability::kVanished:
      return vanished_;
    case topology::Availability::kDenied:
      return denied_;
    case topology::Availability::kMalformed:
      return malformed_;
    case topology::Availability::kUnreadable:
      return unreadable_;
    case topology::Availability::kPresent:
      break;
  }
  return none_;
}

Metric make_metric(std::string_view name, Unit unit, Kind kind, Scope scope, std::uint32_t index,
                   std::string_view provider) {
  Metric metric;
  metric.name = name;
  metric.unit = unit;
  metric.kind = kind;
  metric.scope = scope;
  metric.scope_index = index;
  metric.provider = provider;
  return metric;
}

Unavailable make_unavailable(const topology::Unavailable& failure, const Remedies& remedies) {
  Unavailable out;
  out.kind = failure.kind;
  out.path = failure.path;
  out.detail = failure.detail;
  out.remediation = remedies.for_availability(failure.kind);
  return out;
}

Channel::Channel(platform::FileSystem& filesystem, GaugeSpec spec)
    : spec_(std::move(spec)), source_(filesystem, spec_.path) {}

core::Result<double, topology::Unavailable> Channel::value() {
  auto raw = source_.integer();
  if (!raw.has_value()) {
    return raw.error();
  }
  const std::int64_t number = raw.value();
  if (number < spec_.minimum || number > spec_.maximum) {
    std::string detail = "value " + std::to_string(number) + " is outside the " +
                         std::to_string(spec_.minimum) + " to " + std::to_string(spec_.maximum) +
                         " a sensor of this kind reports";
    return topology::Unavailable{topology::Availability::kMalformed, spec_.path, std::move(detail)};
  }
  return static_cast<double>(number) * spec_.multiplier / spec_.divisor;
}

MetricStatus Channel::probe() {
  MetricStatus status;
  status.metric = spec_.metric;
  auto found = value();
  if (!found.has_value()) {
    status.unavailable = make_unavailable(found.error(), spec_.remedies);
  }
  return status;
}

Reading Channel::read() {
  auto found = value();
  if (found.has_value()) {
    return Gauge{found.value()};
  }
  return make_unavailable(found.error(), spec_.remedies);
}

}  // namespace loadforge::telemetry
