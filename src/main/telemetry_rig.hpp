// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_TELEMETRY_RIG_HPP
#define LOADFORGE_MAIN_TELEMETRY_RIG_HPP

// The telemetry of a run, assembled: every source, the sampler over them, the
// timeline they write, and the capability report the probe produced.
//
// The sources are built here, in one order, over the files of one machine:
// the running one (roots empty and "/sys", "/proc"), or a directory a test
// built. The sources do not agree on what their root is -- thermal, fan,
// frequency and memory take a prefix to /sys and /proc; power and EDAC take
// the sysfs directory itself; counters take the procfs directory -- so the
// one place that knows is here, and nowhere else spells a path.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sampler.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"
#include "telemetry/timeline/bounded_timeline.hpp"

namespace loadforge::cli {

/// Where the machine's files are. All three are empty-prefix-relative to the
/// same tree.
struct MachineRoots {
  std::string prefix;  ///< Before "/sys/..." and "/proc/...": empty on the machine.
  std::string sysfs = "/sys";
  std::string procfs = "/proc";
};

/// The machine's roots under a directory a test built: <dir>/sys, <dir>/proc.
[[nodiscard]] MachineRoots roots_under(const std::string& directory);

/// Every telemetry source, in the order they appear in the report: thermal,
/// fan, frequency, memory, power, counters, edac.
[[nodiscard]] std::vector<std::unique_ptr<telemetry::Source>> make_sources(
    platform::Syscalls& syscalls, platform::FileSystem& filesystem, const MachineRoots& roots);

/// How many points a run's timeline is built to hold: `seconds` of the run
/// and a margin for the start and the stop, sampled every `interval_ns`, over
/// at most `series` series. A bound and not a reservation: nothing is
/// allocated until a point arrives, and a run that outgrows it stops sampling
/// and says so (the timeline refuses, never drops).
[[nodiscard]] std::size_t timeline_capacity(std::int64_t run_ns, std::int64_t interval_ns,
                                            std::size_t series) noexcept;

/// The most series a machine's sources can plausibly register: a fixed number
/// for the machine-wide metrics, and a few per online CPU.
[[nodiscard]] std::size_t series_bound(std::size_t cpus) noexcept;

/// How the sources of a run are made. The command line uses make_sources; a
/// test names one that makes sources it scripts.
using SourceFactory = std::vector<std::unique_ptr<telemetry::Source>> (*)(platform::Syscalls&,
                                                                          platform::FileSystem&,
                                                                          const MachineRoots&);

/// The sampler, its timeline, and what the probe found.
struct Rig {
  explicit Rig(std::size_t capacity) : timeline(capacity) {}
  telemetry::BoundedTimeline timeline;
  std::unique_ptr<telemetry::Sampler> sampler;
  telemetry::CapabilityReport capabilities;
};

/// Builds the sources over `roots`, the timeline for `capacity` points and the
/// sampler, and probes: the capability report is in the rig, and the series
/// are on the timeline. The refusal is the timeline's, when it refuses a series.
[[nodiscard]] core::Result<std::unique_ptr<Rig>, telemetry::TimelineError> make_rig(
    platform::Syscalls& syscalls, platform::FileSystem& filesystem, const MachineRoots& roots,
    std::size_t capacity, SourceFactory sources_for = make_sources);

}  // namespace loadforge::cli

#endif
