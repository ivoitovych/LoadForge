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
#include "main/outcome.hpp"
#include "verification/record.hpp"

namespace loadforge::cli {

namespace {

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

std::vector<PhaseWindow> phase_windows(const telemetry::TimelineView& timeline,
                                       std::int64_t end_ns) {
  const std::span<const telemetry::PhaseMark> marks = timeline.phases();
  std::vector<PhaseWindow> windows(marks.size());
  for (std::size_t at = 0; at < marks.size(); ++at) {
    windows[at].label = marks[at].label;
    windows[at].window = statistics::phase_window(marks, at, end_ns);
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
        PhaseDelta delta;
        delta.phase = phase.label;
        delta.metric = series_name(summary.metric);
        delta.delta = counter->delta;
        deltas.push_back(std::move(delta));
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

/// One line for every EDAC counter the run did not read throughout: it reset or
/// dropped out, so the verdict is incomplete there and the counts are a lower bound.
std::string edac_blind_notes(const results::RunResult& result) {
  std::ostringstream text;
  for (const statistics::MetricSummary& summary : result.signature.metrics) {
    if (summary.metric.name != results::kCorrectedErrorsMetric &&
        summary.metric.name != results::kUncorrectedErrorsMetric) {
      continue;
    }
    const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value);
    const std::uint64_t resets = counter != nullptr ? counter->resets : 0;
    if (resets > 0 || summary.unavailable_samples > 0) {
      text << "edac: " << series_name(summary.metric) << " was not read throughout (" << resets
           << " reset(s), " << summary.unavailable_samples
           << " unavailable sample(s)): the verdict is incomplete there\n";
    }
  }
  return text.str();
}

}  // namespace

std::string summary_text(const results::RunResult& result, const controller::RunReport& report,
                         const telemetry::TimelineView& timeline,
                         const statistics::Summarizer& summarizer,
                         const std::vector<results::FingerprintGap>& gaps) {
  std::ostringstream text;
  text << "verdict: " << results::describe(result.verdict) << "\n";
  if (report.temperature_stop.has_value()) {
    // A run cut short is not a pass whatever its verdict says: its exit code is 7.
    text << "ended by the temperature ceiling: " << controller::describe(*report.temperature_stop)
         << "\n";
  }
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
  std::size_t edac_series = 0;
  for (std::size_t series = 0; series < timeline.series_count(); ++series) {
    const std::string& name = timeline.metric(static_cast<telemetry::SeriesId>(series)).name;
    edac_series += static_cast<std::size_t>(name == results::kCorrectedErrorsMetric) +
                   static_cast<std::size_t>(name == results::kUncorrectedErrorsMetric);
  }
  if (edac_series > 0) {
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
    text << edac_blind_notes(result);
  }
  for (const results::FingerprintGap& gap : gaps) {
    text << "fingerprint: " << gap.field << " not recorded: " << topology::describe(gap.why)
         << "\n";
  }
  return text.str();
}

core::Result<results::RunResult, std::string> assemble_result(const ResultInputs& inputs) {
  const controller::RunReport& report = *inputs.report;
  // What the workers did, as the results' types hold it: a workload's tally,
  // its findings with what isolation made of them, a worker's CPU.
  results::RunResult result = to_run_result(report, *inputs.config);
  result.fingerprint = inputs.fingerprint;
  result.capabilities = inputs.capabilities;
  if (report.sampling.rounds > 0) {
    auto signature = inputs.summarizer->summarize(
        *inputs.timeline,
        statistics::run_window(report.sampling.first_round_ns, report.sampling.last_round_ns));
    if (!signature.has_value()) {
      return std::string{statistics::describe(signature.error().fault)} + ": " +
             signature.error().detail;
    }
    result.signature = std::move(signature).value();
  }
  // A run with no verifying worker (the null workload) reports each configured
  // workload as idle, as M3 did; one with them reports what they verified.
  if (result.workloads.empty()) {
    for (const config::Workload& workload : inputs.config->workloads) {
      if (workload.enabled) {
        result.workloads.push_back(idle_workload(workload));
      }
    }
  }
  result.observers.sample_interval_ns = report.sampling.interval_ns;
  if (report.sampling.enabled && report.elapsed_ns > 0) {
    // Wall time spent in rounds over the run's: an upper bound on the share of
    // one CPU the observer took, since a round that waits on a file is not
    // running.
    const auto spent = static_cast<double>(report.sampling.cost_ns);
    const auto elapsed = static_cast<double>(report.elapsed_ns);
    result.observers.controller_overhead = std::clamp(spent / elapsed, 0.0, 1.0);
  }
  results::seal(result);
  return result;
}

}  // namespace loadforge::cli
