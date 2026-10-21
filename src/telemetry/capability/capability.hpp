// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_CAPABILITY_CAPABILITY_HPP
#define LOADFORGE_TELEMETRY_CAPABILITY_CAPABILITY_HPP

#include <string>

#include "telemetry/source.hpp"

namespace loadforge::telemetry {

// THE SAMPLER PROBES, THIS MODULE RENDERS.
// The capability report IS the result of the sampler's probe() (telemetry/
// sampler.hpp): that call is the only one in the product that calls
// Source::probe(), because it also registers the present metrics as series, so
// a second prober could only repeat it. A source is probed once
// (telemetry/source.hpp), and the report this module renders is read from the
// sampler's answer. (There was a probe_capabilities() here that probed the
// sources itself; a controller that used both probed every source twice. It is
// removed -- docs/journal/m3-contract-corrections-*.md.)

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
