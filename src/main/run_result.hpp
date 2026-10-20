// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_RUN_RESULT_HPP
#define LOADFORGE_MAIN_RUN_RESULT_HPP

// What a run comes to, as the result the reporters write (PLAN F12,
// IMPLEMENTATION M3). The controller reports what its workers did and what its
// sampler took; this puts them together with the machine's fingerprint, the
// capability report and the statistics over the timeline, and seals the whole
// with its verdict.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/results/fingerprint.hpp"
#include "controller/results/result.hpp"
#include "core/result.hpp"
#include "statistics/signature.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::cli {

/// Everything the result is made of that the controller's report is not.
struct ResultInputs {
  const config::Config* config = nullptr;
  const controller::RunReport* report = nullptr;
  results::Fingerprint fingerprint;
  telemetry::CapabilityReport capabilities;
  const telemetry::TimelineView* timeline = nullptr;
  const statistics::Summarizer* summarizer = nullptr;
};

/// The window the whole run's signature is taken over: from the first round to
/// one nanosecond past the last, because a window excludes its end and the
/// last round is the run's closing reading.
[[nodiscard]] statistics::Window run_window(const controller::SamplingReport& sampling) noexcept;

/// The windows of the phases the timeline marked, each from its mark to one
/// nanosecond past the next mark -- the boundary round belongs to both
/// neighbours -- and the last to one nanosecond past `end_ns`.
///
/// Why overlapping: a counter's advance between two consecutive readings
/// belongs to the interval they span. With windows that meet exactly, the
/// reading on the boundary is in the later window only, and the advance
/// across the boundary -- the pair (last reading before it, the reading on
/// it) -- is in neither: the per-phase deltas then sum to less than the run's.
/// With the boundary round in both, every pair is in exactly one phase.
struct PhaseWindow {
  std::string label;
  statistics::Window window;

  [[nodiscard]] friend bool operator==(const PhaseWindow&, const PhaseWindow&) = default;
};
[[nodiscard]] std::vector<PhaseWindow> phase_windows(const telemetry::TimelineView& timeline,
                                                     std::int64_t end_ns);

/// What one counter metric advanced in one phase.
struct PhaseDelta {
  std::string phase;
  std::string metric;  ///< "name" and, for a scope other than the machine, " <scope> <index>".
  std::uint64_t delta = 0;
};

/// The advance of every counter series named `name` in every phase (F13: a
/// corrected error is reported in the phase it happened in). A series with no
/// movement and a series that is unavailable in a phase are not listed:
/// absence is the signature's to report.
[[nodiscard]] std::vector<PhaseDelta> counter_by_phase(const telemetry::TimelineView& timeline,
                                                       const statistics::Summarizer& summarizer,
                                                       std::string_view name, std::int64_t end_ns);

/// The lines the console prints when a run ends, after the controller's own
/// (report.hpp): the verdict, what watched the run and what it cost (F17),
/// what telemetry each dead worker left behind, EDAC counts per phase (F13),
/// and the fingerprint fields the machine would not give.
[[nodiscard]] std::string summary_text(const results::RunResult& result,
                                       const controller::RunReport& report,
                                       const telemetry::TimelineView& timeline,
                                       const statistics::Summarizer& summarizer,
                                       const std::vector<results::FingerprintGap>& gaps);

/// The result of a run. kWindow from the summarizer is the only fault: a
/// window the sampler's own rounds make non-empty, so it means the sampler
/// reported rounds it did not take.
[[nodiscard]] core::Result<results::RunResult, std::string> assemble_result(
    const ResultInputs& inputs);

}  // namespace loadforge::cli

#endif
