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
constexpr std::uint64_t kMarginRounds = 8;
constexpr std::size_t kMachineSeries = 64;
constexpr std::size_t kSeriesPerCpu = 8;
}  // namespace

MachineRoots roots_under(const std::string& directory) {
  MachineRoots roots;
  roots.prefix = directory;
  roots.sysfs = directory;
  roots.sysfs += "/sys";
  roots.procfs = directory;
  roots.procfs += "/proc";
  return roots;
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

std::size_t timeline_capacity(const RunShape& shape) noexcept {
  // Unsigned, because a run that can take the largest signed length plus the
  // margin overflows a signed sum: a quotient of at most 2^63 plus eight fits.
  const auto step = static_cast<std::uint64_t>(std::max<std::int64_t>(shape.interval_ns, 1));
  const auto run = static_cast<std::uint64_t>(std::max<std::int64_t>(shape.run_ns, 0));
  const std::uint64_t rounds = (run / step) + kMarginRounds;
  // run_capacity takes seconds and hertz; rounds are the product of the two,
  // so ask for `rounds` seconds at one round a second.
  return telemetry::run_capacity(static_cast<std::size_t>(rounds), 1, shape.series);
}

core::Result<core::Ok, telemetry::TimelineError> Rig::start(
    std::vector<std::unique_ptr<telemetry::Source>> sources) {
  sampler_ = telemetry::make_sampler(std::move(sources), timeline_);
  auto report = sampler_->probe();
  if (!report.has_value()) {
    return report.error();
  }
  capabilities_ = std::move(report).value();
  return core::Ok{};
}

core::Result<std::unique_ptr<Rig>, telemetry::TimelineError> make_rig(
    platform::Syscalls& syscalls, platform::FileSystem& filesystem, const MachineRoots& roots,
    std::size_t capacity, SourceFactory sources_for) {
  auto rig = std::make_unique<Rig>(capacity);
  if (auto started = rig->start(sources_for(syscalls, filesystem, roots)); !started.has_value()) {
    return started.error();
  }
  return rig;
}

}  // namespace loadforge::cli
