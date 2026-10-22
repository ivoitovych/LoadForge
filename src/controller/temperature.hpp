// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_TEMPERATURE_HPP
#define LOADFORGE_CONTROLLER_TEMPERATURE_HPP

// The temperature ceiling (`limits.max_temperature_c`): which readings on the
// timeline are temperatures, and when one of them ends a run.
//
// The ceiling is enforced from what the sampler has already put on the
// timeline, not from a second read of the sensors: the sampler is the only
// reader of a source (telemetry/sampler.hpp), and a reading the report shows
// is then the reading that stopped the run. After each round the controller
// asks the watch to look at the point every temperature series just received.
//
// WHAT COUNTS AS A TEMPERATURE
// ----------------------------
// Every series of the timeline whose metric is a gauge in degrees Celsius,
// whichever source made it: today the thermal source's `thermal.package`,
// `thermal.core`, `thermal.sensor` and `thermal.zone`. The ceiling names no
// sensor, because nothing guesses which one is "the" package temperature
// (docs/PLAN.md F3): any sensor at or above the ceiling is a machine too hot
// for the limit the user set, VRM and chipset sensors included.
//
// WHAT HAPPENS WHEN THE SENSORS GO
// --------------------------------
// A sensor that stops answering is told once and the others are still watched.
// When none of them has answered for `blind_after_ns` the ceiling can no longer
// be enforced, and a run that cannot enforce a limit it was asked to enforce
// ends the way a run that crossed it does (docs/safety.md).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "telemetry/timeline.hpp"

namespace loadforge::controller {

/// One temperature gauge the ceiling watches.
struct Sensor {
  telemetry::SeriesId series = 0;
  std::string metric;    ///< "thermal.core".
  std::string name;      ///< "thermal.core 3": the metric, and its index unless machine-wide.
  std::string provider;  ///< "hwmon:coretemp", "thermal_zone:x86_pkg_temp".
  bool readable = false;
};

enum class StopCause : std::uint8_t {
  kReached,    ///< A sensor read at or above the ceiling.
  kUnwatched,  ///< No sensor could be read for too long: the ceiling is no longer enforced.
};

/// Why a run was ended by the ceiling, and when.
struct TemperatureStop {
  StopCause cause = StopCause::kReached;
  std::string sensor;    ///< kReached: the sensor, with its provider.
  double celsius = 0.0;  ///< kReached: what it read.
  std::int64_t ceiling_c = 0;
  std::int64_t at_ns = 0;     ///< Into the run (the controller's elapsed time).
  std::string detail;         ///< kUnwatched: why the last sensor could not be read.
  std::int64_t blind_ns = 0;  ///< kUnwatched: for how long no sensor was read.

  /// Defined once, in temperature.cpp (docs/JOURNAL.md section 1.22).
  friend bool operator==(const TemperatureStop& lhs, const TemperatureStop& rhs) noexcept;
};

/// A stop of kReached without its time: "thermal.core 3 (hwmon:coretemp) read
/// 91.0 C, at or above the ceiling of 90 C".
[[nodiscard]] std::string describe_reading(const TemperatureStop& stop);

/// One sentence, for the console, the journal's reader and the result:
/// "thermal.core 3 (hwmon:coretemp) read 91.0 C, at or above the ceiling of
/// 90 C, 12.345 s into the run".
[[nodiscard]] std::string describe(const TemperatureStop& stop);

/// What a round showed.
struct Observation {
  /// Sensors that stopped answering or answer again, in the controller's words.
  std::vector<std::string> notes;
  std::optional<TemperatureStop> stop;
};

class TemperatureWatch {
 public:
  /// Finds the temperature series of `view` and reads the baseline round, which
  /// was taken at `baseline_ns`. `view` must outlive the watch.
  // Degrees, then two lengths of time on the one clock, each named at the one call.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  TemperatureWatch(const telemetry::TimelineView& view, std::int64_t ceiling_c,
                   std::int64_t blind_after_ns, std::int64_t baseline_ns);

  [[nodiscard]] std::int64_t ceiling_c() const noexcept { return ceiling_c_; }
  [[nodiscard]] const std::vector<Sensor>& sensors() const noexcept { return sensors_; }
  /// How many sensors gave a reading in the latest round looked at.
  [[nodiscard]] std::size_t readable() const noexcept;
  /// The sensors by name, with a count where a name has several:
  /// "thermal.package 0, thermal.core x4".
  [[nodiscard]] std::string describe_sensors() const;
  /// Why the latest sensor that could not be read could not be, in the source's words.
  [[nodiscard]] const std::string& last_reason() const noexcept { return last_reason_; }
  /// The baseline, if a sensor was already at or above the ceiling before any
  /// worker started.
  [[nodiscard]] const std::optional<TemperatureStop>& hot_at_start() const noexcept {
    return hot_at_start_;
  }

  /// Looks at the point each temperature series received in the round taken at
  /// `round_ns`, `elapsed_ns` into the run.
  // Two times on two clocks, named for what they are.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] Observation observe(std::int64_t round_ns, std::int64_t elapsed_ns);

 private:
  const telemetry::TimelineView* view_;
  std::int64_t ceiling_c_;
  std::int64_t blind_after_ns_;
  std::int64_t last_read_ns_;
  std::string last_reason_;
  std::vector<Sensor> sensors_;
  std::optional<TemperatureStop> hot_at_start_;
};

}  // namespace loadforge::controller

#endif
