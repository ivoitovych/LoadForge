// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/thermal/hwmon.hpp"

#include <string>
#include <utility>

#include "topology/source.hpp"

namespace loadforge::telemetry {

std::vector<HwmonChip> find_hwmon_chips(platform::FileSystem& filesystem, std::string_view root) {
  std::vector<HwmonChip> chips;
  for (std::uint32_t index = 0; index < kMaxHwmonChips; ++index) {
    std::string directory = std::string{root} + "/sys/class/hwmon/hwmon" + std::to_string(index);
    topology::Source name_file(filesystem, directory + "/name");
    auto name = name_file.text();
    // Filled in place rather than pushed as a HwmonChip{...} temporary: two
    // allocating members built in one expression leave a cleanup branch no test
    // can take (docs/JOURNAL.md 1.13).
    if (name.has_value()) {
      HwmonChip& chip = chips.emplace_back();
      chip.directory = std::move(directory);
      chip.index = index;
      chip.name = name.value();
    } else if (name.error().kind != topology::Availability::kAbsent) {
      // Present but unreadable: the chip is there and its sensors may be
      // readable, so it is kept, unnamed. Only a chip that is not there is
      // skipped.
      HwmonChip& chip = chips.emplace_back();
      chip.directory = std::move(directory);
      chip.index = index;
    }
  }
  return chips;
}

std::string hwmon_provider(const HwmonChip& chip) {
  if (chip.name.empty()) {
    return "hwmon:hwmon" + std::to_string(chip.index);
  }
  return "hwmon:" + chip.name;
}

std::string read_label(platform::FileSystem& filesystem, const std::string& path) {
  topology::Source label_file(filesystem, path);
  auto label = label_file.text();
  if (label.has_value()) {
    return label.value();
  }
  return std::string{};
}

}  // namespace loadforge::telemetry
