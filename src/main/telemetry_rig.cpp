// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/telemetry_rig.hpp"

#include <algorithm>
#include <utility>

#include "telemetry/sources/counters/counters.hpp"
#include "telemetry/sources/edac/edac.hpp"
#include "telemetry/sources/fan/fan.hpp"
#include "telemetry/sources/frequency/frequency.hpp"
#include "telemetry/sources/memory/memory.hpp"
#include "telemetry/sources/power/power.hpp"
#include "telemetry/sources/thermal/thermal.hpp"

namespace loadforge::cli {

namespace {
/// Beyond the run's own length: the baseline round, the round at the deadline,
/// the stop's grace and the round after the last reap.
constexpr std::int64_t kMarginRounds = 8;
constexpr std::size_t kMachineSeries = 64;
constexpr std::size_t kSeriesPerCpu = 8;
}  // namespace

MachineRoots roots_under(const std::string& directory) {
  return MachineRoots{directory, directory + "/sys", directory + "/proc"};
}

std::vector<std::unique_ptr<telemetry::Source>> make_sources(platform::Syscalls& syscalls,
                                                             platform::FileSystem& filesystem,
                                                             const MachineRoots& roots) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<telemetry::ThermalSource>(filesystem, roots.prefix));
  sources.push_back(std::make_unique<telemetry::FanSource>(filesystem, roots.prefix));
  sources.push_back(std::make_unique<telemetry::FrequencySource>(filesystem, roots.prefix));
  sources.push_back(std::make_unique<telemetry::MemorySource>(filesystem, roots.prefix));
  sources.push_back(std::make_unique<telemetry::PowerSource>(syscalls, filesystem, roots.sysfs));
  sources.push_back(
      std::make_unique<telemetry::CountersSource>(syscalls, filesystem, roots.procfs));
  sources.push_back(std::make_unique<telemetry::EdacSource>(filesystem, roots.sysfs));
  return sources;
}

std::size_t series_bound(std::size_t cpus) noexcept {
  return kMachineSeries + (kSeriesPerCpu * cpus);
}

std::size_t timeline_capacity(std::int64_t run_ns, std::int64_t interval_ns,
                              std::size_t series) noexcept {
  const std::int64_t step = std::max<std::int64_t>(interval_ns, 1);
  const std::int64_t rounds = (std::max<std::int64_t>(run_ns, 0) / step) + kMarginRounds;
  // run_capacity takes seconds and hertz; rounds are the product of the two,
  // so ask for `rounds` seconds at one round a second.
  return telemetry::run_capacity(static_cast<std::size_t>(rounds), 1, series);
}

core::Result<std::unique_ptr<Rig>, telemetry::TimelineError> make_rig(
    platform::Syscalls& syscalls, platform::FileSystem& filesystem, const MachineRoots& roots,
    std::size_t capacity) {
  auto rig = std::make_unique<Rig>(capacity);
  rig->sampler = telemetry::make_sampler(make_sources(syscalls, filesystem, roots), rig->timeline);
  auto report = rig->sampler->probe();
  if (!report.has_value()) {
    return report.error();
  }
  rig->capabilities = std::move(report).value();
  return rig;
}

}  // namespace loadforge::cli
