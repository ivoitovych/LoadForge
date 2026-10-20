// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_HWMON_TREE_HPP
#define LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_HWMON_TREE_HPP

// LOADFORGE P7 FIXTURE BUILDER
//
// The hwmon and thermal-zone layouts the telemetry sources read, built in a
// MemFs. No machine reachable from this stream had an hwmon chip, a thermal
// zone, a fan or a cpufreq directory (tests/fixtures/sysfs/README.md), so the
// present states cannot be captured and are built, in the layout the kernel's
// Documentation/hwmon/sysfs-interface.rst and Documentation/driver-api/thermal/
// sysfs-api.rst give: <attribute> files holding one decimal integer and a
// newline, `name` and `*_label` holding one line of text.
//
// What a real machine would replace these with is in the journal fragment.

#include <string>

#include "unit/telemetry/sources/thermal/memfs.hpp"

namespace loadforge::telemetry::fixtures {

inline std::string hwmon_dir(int chip) {
  return "/sys/class/hwmon/hwmon" + std::to_string(chip);
}

inline void add_chip(MemFs& fs, int chip, const std::string& name) {
  fs.set(hwmon_dir(chip) + "/name", name + "\n");
}

/// tempN_input in millidegrees, and tempN_label when `label` is not empty.
inline void add_temp(MemFs& fs, int chip, int input, const std::string& milli,
                     const std::string& label = {}) {
  const std::string stem = hwmon_dir(chip) + "/temp" + std::to_string(input);
  fs.set(stem + "_input", milli + "\n");
  if (!label.empty()) {
    fs.set(stem + "_label", label + "\n");
  }
}

inline std::string temp_path(int chip, int input) {
  return hwmon_dir(chip) + "/temp" + std::to_string(input) + "_input";
}

/// fanN_input in rpm.
inline void add_fan(MemFs& fs, int chip, int input, const std::string& rpm) {
  fs.set(hwmon_dir(chip) + "/fan" + std::to_string(input) + "_input", rpm + "\n");
}

inline std::string fan_path(int chip, int input) {
  return hwmon_dir(chip) + "/fan" + std::to_string(input) + "_input";
}

/// /sys/class/thermal/thermal_zoneN/{type,temp}.
inline void add_zone(MemFs& fs, int zone, const std::string& type, const std::string& milli) {
  const std::string dir = "/sys/class/thermal/thermal_zone" + std::to_string(zone);
  if (!type.empty()) {
    fs.set(dir + "/type", type + "\n");
  }
  fs.set(dir + "/temp", milli + "\n");
}

inline std::string zone_path(int zone) {
  return "/sys/class/thermal/thermal_zone" + std::to_string(zone) + "/temp";
}

}  // namespace loadforge::telemetry::fixtures

#endif
