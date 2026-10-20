// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/run_result.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <span>
#include <sstream>
#include <utility>
#include <variant>

#include "controller/results/aggregate.hpp"
#include "verification/record.hpp"

namespace loadforge::cli {

namespace {

constexpr std::int64_t kWindowEndMargin = 1;

std::string series_name(const telemetry::Metric& metric) {
  std::string text = metric.name;
  if (metric.scope != telemetry::Scope::kMachine) {
    text += " ";
    text += telemetry::describe(metric.scope);
    text += " ";
    text += std::to_string(metric.scope_index);
  }
  return text;
}

/// The run as the controller saw it, one workload result per workload it ran.
/// The worker's null workload computes and checks nothing, so its tally is
/// zero and its duty cycle says no check is scheduled: a result that claimed a
/// period of 1 would claim a check that never happens.
results::WorkloadResult idle_workload(const config::Workload& workload) {
  results::WorkloadResult result;
  result.descriptor.name = workload.name;
  result.duty = verification::DutyCycle{0, 0};
  return result;
}

}  // namespace

statistics::Window run_window(const controller::SamplingReport& sampling) noexcept {
  return statistics::Window{sampling.first_round_ns, sampling.last_round_ns + kWindowEndMargin};
}

std::vector<PhaseWindow> phase_windows(const telemetry::TimelineView& timeline,
                                       std::int64_t end_ns) {
  const std::span<const telemetry::PhaseMark> marks = timeline.phases();
  std::vector<PhaseWindow> windows;
  windows.reserve(marks.size());
  for (std::size_t at = 0; at < marks.size(); ++at) {
    const bool last = at + 1 == marks.size();
    const std::int64_t end = last ? end_ns : marks[at + 1].start_ns;
    windows.push_back(PhaseWindow{marks[at].label, {marks[at].start_ns, end + kWindowEndMargin}});
  }
  return windows;
}

std::vector<PhaseDelta> counter_by_phase(const telemetry::TimelineView& timeline,
                                         const statistics::Summarizer& summarizer,
                                         std::string_view name, std::int64_t end_ns) {
  std::vector<PhaseDelta> deltas;
  for (const PhaseWindow& phase : phase_windows(timeline, end_ns)) {
    const auto signature = summarizer.summarize(timeline, phase.window);
    if (!signature.has_value()) {
      continue;
    }
    for (const statistics::MetricSummary& summary : signature.value().metrics) {
      const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value);
      if (summary.metric.name == name && counter != nullptr && counter->delta > 0) {
        deltas.push_back(PhaseDelta{phase.label, series_name(summary.metric), counter->delta});
      }
    }
  }
  return deltas;
}

namespace {

std::string seconds(std::int64_t nanoseconds) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(3) << static_cast<double>(nanoseconds) / 1e9 << " s";
  return text.str();
}

/// Points at or before `at_ns`, over every series: what the timeline held of a
/// worker's run when it died.
std::size_t points_until(const telemetry::TimelineView& timeline, std::int64_t at_ns) {
  std::size_t count = 0;
  for (std::size_t series = 0; series < timeline.series_count(); ++series) {
    for (const telemetry::Point& point :
         timeline.points(static_cast<telemetry::SeriesId>(series))) {
      if (point.timestamp_ns <= at_ns) {
        ++count;
      }
    }
  }
  return count;
}

}  // namespace

std::string summary_text(const results::RunResult& result, const controller::RunReport& report,
                         const telemetry::TimelineView& timeline,
                         const statistics::Summarizer& summarizer,
                         const std::vector<results::FingerprintGap>& gaps) {
  std::ostringstream text;
  text << "verdict: " << results::describe(result.verdict) << "\n";
  const controller::SamplingReport& sampling = report.sampling;
  if (!sampling.enabled) {
    text << "telemetry: not sampled\n";
  } else {
    text << "telemetry: " << sampling.rounds << " round(s) every " << seconds(sampling.interval_ns)
         << ", " << sampling.points << " point(s) over " << timeline.series_count()
         << " series; sampling took " << seconds(sampling.cost_ns);
    if (result.observers.controller_overhead.has_value()) {
      text << " (" << std::fixed << std::setprecision(4)
           << *result.observers.controller_overhead * 100.0 << " % of one CPU)";
    }
    text << "\n";
  }
  if (sampling.fault.has_value()) {
    text << "telemetry fault: " << *sampling.fault << "\n";
  }
  for (const controller::Reap& reap : sampling.reaps) {
    text << "worker " << reap.slot << " reaped at " << seconds(reap.at_ns - sampling.first_round_ns)
         << " into the telemetry; the timeline holds " << points_until(timeline, reap.at_ns)
         << " point(s) up to then\n";
  }
  bool edac = false;
  for (std::size_t series = 0; series < timeline.series_count(); ++series) {
    const std::string& name = timeline.metric(static_cast<telemetry::SeriesId>(series)).name;
    edac = edac || name == results::kCorrectedErrorsMetric ||
           name == results::kUncorrectedErrorsMetric;
  }
  if (edac) {
    std::vector<PhaseDelta> deltas;
    for (const std::string_view name :
         {results::kCorrectedErrorsMetric, results::kUncorrectedErrorsMetric}) {
      for (PhaseDelta& delta :
           counter_by_phase(timeline, summarizer, name, sampling.last_round_ns)) {
        deltas.push_back(std::move(delta));
      }
    }
    for (const PhaseDelta& delta : deltas) {
      text << "edac: " << delta.metric << " advanced by " << delta.delta << " in phase \""
           << delta.phase << "\"\n";
    }
    if (deltas.empty()) {
      text << "edac: no error counted in any phase\n";
    }
  }
  for (const results::FingerprintGap& gap : gaps) {
    text << "fingerprint: " << gap.field << " not recorded: " << topology::describe(gap.why)
         << "\n";
  }
  return text.str();
}

core::Result<results::RunResult, std::string> assemble_result(const ResultInputs& inputs) {
  const controller::RunReport& report = *inputs.report;
  results::RunResult result;
  result.mode = inputs.config->run.mode;
  result.elapsed_ns = report.elapsed_ns;
  result.fingerprint = inputs.fingerprint;
  result.capabilities = inputs.capabilities;
  if (report.sampling.rounds > 0) {
    auto signature = inputs.summarizer->summarize(*inputs.timeline, run_window(report.sampling));
    if (!signature.has_value()) {
      return std::string{statistics::describe(signature.error().fault)} + ": " +
             signature.error().detail;
    }
    result.signature = std::move(signature).value();
  }
  for (const config::Workload& workload : inputs.config->workloads) {
    if (workload.enabled) {
      result.workloads.push_back(idle_workload(workload));
    }
  }
  for (const controller::WorkerReport& worker : report.workers) {
    result.workers.push_back(
        results::WorkerResult{worker.slot, worker.fate, std::nullopt, worker.termination});
  }
  result.observers.sample_interval_ns = report.sampling.interval_ns;
  if (report.sampling.enabled && report.elapsed_ns > 0) {
    result.observers.controller_overhead = std::clamp(
        static_cast<double>(report.sampling.cost_ns) / static_cast<double>(report.elapsed_ns), 0.0,
        1.0);
  }
  results::seal(result);
  return result;
}

}  // namespace loadforge::cli
