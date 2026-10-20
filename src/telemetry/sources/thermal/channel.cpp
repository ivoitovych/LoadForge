// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/thermal/channel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "core/result.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

Remedies make_remedies(std::string absent, std::string vanished, std::string denied,
                       std::string malformed, std::string unreadable) {
  Remedies remedies;
  remedies.by_availability[static_cast<std::size_t>(topology::Availability::kAbsent)] =
      std::move(absent);
  remedies.by_availability[static_cast<std::size_t>(topology::Availability::kVanished)] =
      std::move(vanished);
  remedies.by_availability[static_cast<std::size_t>(topology::Availability::kDenied)] =
      std::move(denied);
  remedies.by_availability[static_cast<std::size_t>(topology::Availability::kMalformed)] =
      std::move(malformed);
  remedies.by_availability[static_cast<std::size_t>(topology::Availability::kUnreadable)] =
      std::move(unreadable);
  return remedies;
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
    return topology::Unavailable{topology::Availability::kMalformed, spec_.path,
                                 std::move(detail)};
  }
  return static_cast<double>(number) * spec_.multiplier / spec_.divisor;
}

MetricStatus Channel::probe() {
  auto found = value();
  if (found.has_value()) {
    return MetricStatus{spec_.metric, std::nullopt};
  }
  return MetricStatus{spec_.metric, make_unavailable(found.error(), spec_.remedies)};
}

Reading Channel::read() {
  auto found = value();
  if (found.has_value()) {
    return Gauge{found.value()};
  }
  return make_unavailable(found.error(), spec_.remedies);
}

}  // namespace loadforge::telemetry
