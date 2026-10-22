// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/temperature.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "telemetry/sample.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::controller {

namespace {

constexpr double kNanosecondsPerSecond = 1e9;

std::string fixed(double value, int places) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(places) << value;
  return text.str();
}

std::string seconds(std::int64_t nanoseconds, int places) {
  return fixed(static_cast<double>(nanoseconds) / kNanosecondsPerSecond, places);
}

/// The sensor as the reports name it: "thermal.core 3 (hwmon:coretemp)".
std::string with_provider(const Sensor& sensor) {
  std::string text = sensor.name;
  text += " (";
  text += sensor.provider;
  text += ")";
  return text;
}

/// Why a reading is not a temperature.
std::string reason_of(const telemetry::Reading& reading) {
  if (const auto* gone = std::get_if<telemetry::Unavailable>(&reading)) {
    return gone->detail;
  }
  return "the reading is not a temperature";
}

}  // namespace

bool operator==(const TemperatureStop& lhs, const TemperatureStop& rhs) noexcept {
  return std::tie(lhs.cause, lhs.sensor, lhs.celsius, lhs.ceiling_c, lhs.at_ns, lhs.detail,
                  lhs.blind_ns) == std::tie(rhs.cause, rhs.sensor, rhs.celsius, rhs.ceiling_c,
                                            rhs.at_ns, rhs.detail, rhs.blind_ns);
}

std::string describe_reading(const TemperatureStop& stop) {
  std::string text = stop.sensor;
  text += " read ";
  text += fixed(stop.celsius, 1);
  text += " C, at or above the ceiling of ";
  text += std::to_string(stop.ceiling_c);
  text += " C";
  return text;
}

std::string describe(const TemperatureStop& stop) {
  std::string text;
  if (stop.cause == StopCause::kReached) {
    text = describe_reading(stop);
    text += ", ";
  } else {
    text = "no temperature could be read for ";
    text += seconds(stop.blind_ns, 1);
    text += " s (";
    text += stop.detail;
    text += "), so the ceiling of ";
    text += std::to_string(stop.ceiling_c);
    text += " C can no longer be enforced, ";
  }
  text += seconds(stop.at_ns, 3);
  text += " s into the run";
  return text;
}

TemperatureWatch::TemperatureWatch(const telemetry::TimelineView& view, std::int64_t ceiling_c,
                                   std::int64_t blind_after_ns, std::int64_t baseline_ns)
    : view_(&view),
      ceiling_c_(ceiling_c),
      blind_after_ns_(blind_after_ns),
      last_read_ns_(baseline_ns) {
  for (std::size_t at = 0; at < view.series_count(); ++at) {
    const auto id = static_cast<telemetry::SeriesId>(at);
    const telemetry::Metric& metric = view.metric(id);
    if (metric.unit != telemetry::Unit::kCelsius || metric.kind != telemetry::Kind::kGauge) {
      continue;
    }
    Sensor sensor;
    sensor.series = id;
    sensor.metric = metric.name;
    sensor.name = metric.name;
    if (metric.scope != telemetry::Scope::kMachine) {
      sensor.name += " ";
      sensor.name += std::to_string(metric.scope_index);
    }
    sensor.provider = metric.provider;
    sensor.readable = true;  // Optimistic: the baseline below sets what is true, silently.
    sensors_.push_back(std::move(sensor));
  }
  Observation baseline = observe(baseline_ns, 0);
  hot_at_start_ = std::move(baseline.stop);
}

std::size_t TemperatureWatch::readable() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(sensors_, [](const Sensor& sensor) { return sensor.readable; }));
}

std::string TemperatureWatch::describe_sensors() const {
  std::vector<std::string> metrics;
  for (const Sensor& sensor : sensors_) {
    if (std::ranges::find(metrics, sensor.metric) == metrics.end()) {
      metrics.push_back(sensor.metric);
    }
  }
  std::string text;
  for (const std::string& metric : metrics) {
    const auto same = [&metric](const Sensor& sensor) { return sensor.metric == metric; };
    const auto count = std::ranges::count_if(sensors_, same);
    if (!text.empty()) {
      text += ", ";
    }
    if (count == 1) {
      text += std::ranges::find_if(sensors_, same)->name;
    } else {
      text += metric;
      text += " x";
      text += std::to_string(count);
    }
  }
  return text;
}

Observation TemperatureWatch::observe(std::int64_t round_ns, std::int64_t elapsed_ns) {
  Observation seen;
  const Sensor* hottest = nullptr;
  double hottest_c = 0.0;
  bool any_read = false;
  for (Sensor& sensor : sensors_) {
    const std::span<const telemetry::Point> points = view_->points(sensor.series);
    // A series that did not receive a point in this round (a round the timeline
    // refused part of the way) has no reading now.
    const bool taken = !points.empty() && points.back().timestamp_ns == round_ns;
    const telemetry::Gauge* gauge =
        taken ? std::get_if<telemetry::Gauge>(&points.back().reading) : nullptr;
    if (gauge == nullptr) {
      last_reason_ = taken ? reason_of(points.back().reading) : "no reading was taken";
      if (sensor.readable) {
        seen.notes.push_back(with_provider(sensor) + " can no longer be read: " + last_reason_);
      }
      sensor.readable = false;
      continue;
    }
    any_read = true;
    if (!sensor.readable) {
      seen.notes.push_back(with_provider(sensor) + " can be read again");
    }
    sensor.readable = true;
    if (gauge->value >= static_cast<double>(ceiling_c_) &&
        (hottest == nullptr || gauge->value > hottest_c)) {
      hottest = &sensor;
      hottest_c = gauge->value;
    }
  }
  if (any_read) {
    last_read_ns_ = round_ns;
  }
  if (hottest != nullptr) {
    TemperatureStop stop;
    stop.cause = StopCause::kReached;
    stop.sensor = with_provider(*hottest);
    stop.celsius = hottest_c;
    stop.ceiling_c = ceiling_c_;
    stop.at_ns = elapsed_ns;
    seen.stop = std::move(stop);
  } else if (!any_read && round_ns - last_read_ns_ > blind_after_ns_) {
    TemperatureStop stop;
    stop.cause = StopCause::kUnwatched;
    stop.ceiling_c = ceiling_c_;
    stop.at_ns = elapsed_ns;
    stop.detail = last_reason_;
    stop.blind_ns = round_ns - last_read_ns_;
    seen.stop = std::move(stop);
  }
  return seen;
}

}  // namespace loadforge::controller
