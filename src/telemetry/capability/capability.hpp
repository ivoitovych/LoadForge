// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_CAPABILITY_CAPABILITY_HPP
#define LOADFORGE_TELEMETRY_CAPABILITY_CAPABILITY_HPP

#include <span>
#include <string>

#include "telemetry/source.hpp"

namespace loadforge::telemetry {

/// Probes every source, once, and gathers what each found into the report
/// (M2a exit criterion 5). The order of `sources` is the order of the report.
/// The sources are probed here and nowhere else: probe() is called once, before
/// any sample(), and the sampler that follows uses the same objects.
[[nodiscard]] CapabilityReport probe_capabilities(std::span<Source* const> sources);

/// The capability set as the console prints it at the start of a run: one
/// block per source, what is present, and for every metric that is not, the
/// state, where, why and the remedy for the provider that failed.
///
/// Metrics of one source that are absent for the same reason with the same
/// remedy (the same sensor on every CPU) are one entry with a count, so a
/// machine with 256 CPUs and no cpufreq prints one entry, not 256.
[[nodiscard]] std::string render_capabilities(const CapabilityReport& report);

}  // namespace loadforge::telemetry

#endif
