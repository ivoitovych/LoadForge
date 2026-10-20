// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_THERMAL_HWMON_HPP
#define LOADFORGE_TELEMETRY_SOURCES_THERMAL_HWMON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"

namespace loadforge::telemetry {

/// How many hwmon chips, and how many numbered inputs of one kind per chip
/// (temp1..temp255, fan1..fan255), discovery looks for.
///
/// The platform seam reads files; it does not list directories, and the
/// frozen syscall interface is not LoadForge's to widen from a telemetry
/// stream. Discovery therefore asks for hwmon0..hwmon63 by name, which is what
/// a directory listing would have shown on every machine measured: the kernel
/// hands out hwmon numbers lowest-free-first, so a chip beyond 63 means a
/// machine with sixty-four registered sensor chips. A machine with more is
/// reported by what is found, and the journal records the bound.
inline constexpr std::uint32_t kMaxHwmonChips = 64;
inline constexpr std::uint32_t kMaxHwmonChannels = 255;

/// One /sys/class/hwmon/hwmonN directory that has a `name` file, or has one
/// the process cannot read.
struct HwmonChip {
  std::string directory;
  std::uint32_t index = 0;
  /// The chip's `name`: "coretemp", "k10temp", "nct6775". Empty when the file
  /// could not be read, which makes the chip's sensors unidentified.
  std::string name;
};

[[nodiscard]] std::vector<HwmonChip> find_hwmon_chips(platform::FileSystem& filesystem,
                                                      std::string_view root);

/// "hwmon:coretemp", or "hwmon:hwmon3" for a chip whose name is unknown.
[[nodiscard]] std::string hwmon_provider(const HwmonChip& chip);

/// A series index that is stable across runs on one machine and unique across
/// chips and channels: the chip's number in the high bits, the input's below.
[[nodiscard]] constexpr std::uint32_t hwmon_series_index(const HwmonChip& chip,
                                                         std::uint32_t channel) noexcept {
  return (chip.index << 8U) | channel;
}

/// The text of a label file, or empty when there is none or it cannot be read.
/// A label is only used to identify a sensor, and a sensor with no usable label
/// is reported unidentified, not hidden.
[[nodiscard]] std::string read_label(platform::FileSystem& filesystem, const std::string& path);

}  // namespace loadforge::telemetry

#endif
