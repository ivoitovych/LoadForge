// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/trial_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "controller/report.hpp"
#include "controller/results/result.hpp"
#include "core/duration.hpp"
#include "main/cli.hpp"
#include "main/run_result.hpp"
#include "platform/clock.hpp"
#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "reporters/console.hpp"
#include "resources/cgroup.hpp"
#include "telemetry/sampler.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::cli {

namespace {

using explore::Candidate;
using explore::Settle;
using explore::TrialError;
using explore::TrialFault;
using explore::TrialOutcome;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;

/// The most worker processes one candidate may ask for: a mistyped count is
/// refused instead of forking until the machine gives out.
constexpr std::uint32_t kMostThreads = 1024;

/// The longest a trial's load, or its settle phase, may be: thirty days. A
/// bound so that the sums of lengths below cannot overflow, and far beyond
/// anything a search runs.
constexpr std::int64_t kLongestNs = std::int64_t{30} * 24 * 3600 * 1'000'000'000;

/// The shortest interval between two rounds of the settle phase.
constexpr std::int64_t kSettleFloorNs = kNanosecondsPerMillisecond;

TrialError refuse(TrialFault fault, std::string detail) {
  return TrialError{fault, std::move(detail)};
}

std::string quoted(const std::string& text) {
  std::string out = "'";
  out += text;
  out += "'";
  return out;
}

/// Why `candidate` cannot be run, or nothing.
std::optional<TrialError> check_candidate(const Candidate& candidate,
                                          const std::vector<std::string>& known,
                                          const std::vector<std::uint32_t>& allowed) {
  const std::string name = quoted(candidate.id);
  if (candidate.workloads.empty()) {
    return refuse(TrialFault::kCandidate, "candidate " + name + " names no workload");
  }
  for (const std::string& workload : candidate.workloads) {
    if (std::ranges::find(known, workload) == known.end()) {
      std::string detail =
          "candidate " + name + ": no workload named " + quoted(workload) + "; this build has:";
      for (const std::string& each : known) {
        detail += " ";
        detail += each;
      }
      return refuse(TrialFault::kCandidate, std::move(detail));
    }
  }
  if (candidate.threads == 0) {
    return refuse(TrialFault::kCandidate, "candidate " + name + " has no threads");
  }
  if (candidate.threads > kMostThreads) {
    return refuse(TrialFault::kCandidate, "candidate " + name + " asks for more than " +
                                              std::to_string(kMostThreads) + " worker processes");
  }
  if (candidate.threads < candidate.workloads.size()) {
    return refuse(TrialFault::kCandidate, "candidate " + name +
                                              " has fewer threads than workloads: a workload would "
                                              "not run");
  }
  if (candidate.duration_ns <= 0 || candidate.duration_ns > kLongestNs) {
    return refuse(TrialFault::kCandidate,
                  "candidate " + name + ": the duration must be above 0 and at most 30 days");
  }
  for (const std::uint32_t cpu : candidate.cpus) {
    if (std::ranges::find(allowed, cpu) == allowed.end()) {
      return refuse(TrialFault::kCandidate, "candidate " + name + ": CPU " + std::to_string(cpu) +
                                                " is not one this process may use");
    }
  }
  if (candidate.settle.has_value()) {
    const Settle& settle = *candidate.settle;
    const bool formed = settle.window_ns > 0 && settle.window_ns <= kLongestNs &&
                        settle.max_ns > 0 && settle.max_ns <= kLongestNs &&
                        settle.max_slope_per_second >= 0.0;
    if (!formed) {
      return refuse(TrialFault::kCandidate,
                    "candidate " + name +
                        ": the settle phase needs a window and a maximum "
                        "above 0 (at most 30 days) and a slope bound of 0 or more");
    }
  }
  return std::nullopt;
}

/// Why the settle metric cannot be measured, or nothing: a gauge of that name
/// must be present in the report, in some scope.
std::optional<TrialError> check_settle_metric(const Settle& settle,
                                              const telemetry::CapabilityReport& report) {
  std::optional<std::string> why;
  for (const telemetry::SourceStatus& source : report.sources) {
    for (const telemetry::MetricStatus& status : source.metrics) {
      if (status.metric.name != settle.metric || status.metric.kind != telemetry::Kind::kGauge) {
        continue;
      }
      if (!status.unavailable.has_value()) {
        return std::nullopt;
      }
      why = status.unavailable->detail + "; " + status.unavailable->remediation;
    }
  }
  std::string detail = "settle metric " + quoted(settle.metric);
  if (why.has_value()) {
    detail += " is not readable on this machine: " + *why;
  } else {
    detail += " is not a gauge in the capability report";
  }
  return refuse(TrialFault::kMetricUnavailable, std::move(detail));
}

/// The settle metric's slope per second over `window`, if the timeline has it.
std::optional<double> slope_of(const statistics::Summarizer& summarizer,
                               const telemetry::TimelineView& timeline,
                               const statistics::Window& window, const std::string& metric) {
  const auto signature = summarizer.summarize(timeline, window);
  if (!signature.has_value()) {
    return std::nullopt;
  }
  for (const statistics::MetricSummary& summary : signature.value().metrics) {
    if (summary.metric.name != metric) {
      continue;
    }
    if (const auto* gauge = std::get_if<statistics::GaugeSummary>(&summary.value)) {
      return gauge->slope_per_second;
    }
  }
  return std::nullopt;
}

/// A trial's whole rig: the telemetry made for it, and its capacity.
std::size_t capacity_for(const Candidate& candidate, const controller::RunOptions& options,
                         std::size_t cpus, std::int64_t settle_interval_ns) {
  const std::int64_t run_ns = candidate.duration_ns + options.grace.stop_ns +
                              options.grace.kill_ns + options.stale_after_ns;
  const std::size_t bound = series_bound(cpus);
  std::size_t capacity = timeline_capacity(RunShape{run_ns, options.sample_interval_ns, bound});
  if (candidate.settle.has_value()) {
    capacity += timeline_capacity(RunShape{candidate.settle->max_ns, settle_interval_ns, bound});
  }
  return capacity;
}

/// The instant the load ended: the controller's "stop" mark, else the last round
/// it took (a run that ended before its deadline never marks one).
std::int64_t deadline_of(const telemetry::TimelineView& timeline,
                         const controller::RunReport& report) {
  const auto phases = timeline.phases();
  const auto stop = std::ranges::find_if(
      phases, [](const telemetry::PhaseMark& mark) { return mark.label == "stop"; });
  if (stop != phases.end()) {
    return stop->start_ns;
  }
  return report.sampling.last_round_ns;
}

std::string describe_statistics(const statistics::StatisticsError& error) {
  std::string text{statistics::describe(error.fault)};
  text += ": ";
  text += error.detail;
  return text;
}

}  // namespace

core::Result<std::unique_ptr<ControllerTrialRunner>, TrialError> ControllerTrialRunner::create(
    platform::Syscalls& syscalls, const topology::Snapshot& machine,
    const workloads::Registry& registry, TrialOptions options) {
  // The probe: every source, once, for the report. A rig of its own, dropped at
  // the end of this function, because each trial makes its own (sources are
  // probed once per rig, and a timeline holds one trial).
  platform::FileSystem files(syscalls);
  auto probed =
      make_rig(syscalls, files, options.roots,
               timeline_capacity(RunShape{0, options.sample_interval_ns,
                                          series_bound(machine.cpus.logical_cpu_count())}),
               options.sources);
  if (!probed.has_value()) {
    std::string detail = "cannot set up telemetry: ";
    detail += telemetry::describe(probed.error().fault);
    detail += ": ";
    detail += probed.error().detail;
    return refuse(TrialFault::kRun, std::move(detail));
  }
  return std::make_unique<ControllerTrialRunner>(syscalls, machine, registry, std::move(options),
                                                 probed.value()->capabilities());
}

ControllerTrialRunner::ControllerTrialRunner(platform::Syscalls& syscalls,
                                             const topology::Snapshot& machine,
                                             const workloads::Registry& registry,
                                             TrialOptions options,
                                             telemetry::CapabilityReport capabilities)
    : syscalls_(&syscalls),
      machine_(&machine),
      registry_(&registry),
      options_(std::move(options)),
      capabilities_(std::move(capabilities)),
      files_(syscalls),
      oom_record_(files_, resources::kMachineRoot),
      isa_probe_(isa::make_native_probe()),
      allowed_cpus_(allowed_cpus(syscalls, machine)) {}

const telemetry::CapabilityReport& ControllerTrialRunner::capabilities() const {
  return capabilities_;
}

std::optional<int> ControllerTrialRunner::worker_exit_code() const noexcept {
  return worker_exit_code_;
}

std::size_t ControllerTrialRunner::trials_started() const noexcept { return trials_; }

core::Result<TrialOutcome, TrialError> ControllerTrialRunner::run(const Candidate& candidate) {
  if (auto refused = check_candidate(candidate, registry_->names(), allowed_cpus_);
      refused.has_value()) {
    return std::move(*refused);
  }
  if (candidate.settle.has_value()) {
    if (auto refused = check_settle_metric(*candidate.settle, capabilities_); refused.has_value()) {
      return std::move(*refused);
    }
  }
  ++trials_;
  std::ostream& log = options_.log != nullptr ? *options_.log : discard_;

  // What the workers are: threads processes, the workloads dealt out in turn.
  config::Config config;
  config.run.mode = config::Mode::kExplore;
  const std::int64_t whole_ms = candidate.duration_ns / kNanosecondsPerMillisecond;
  const std::int64_t part_ms =
      static_cast<std::int64_t>((candidate.duration_ns % kNanosecondsPerMillisecond) != 0);
  config.run.duration = core::Duration{whole_ms + part_ms};
  for (std::size_t slot = 0; slot < candidate.threads; ++slot) {
    config.workloads.emplace_back(candidate.workloads[slot % candidate.workloads.size()],
                                  config::Threads::count(1), true, std::nullopt);
  }

  controller::RunOptions options;
  options.executable = options_.executable;
  options.topology = machine_;
  options.oom_record = &oom_record_;
  allow_for_verified_units(options);
  options.verification.registry = registry_;
  options.verification.seed = options_.seed;
  options.verification.footprint_bytes = options_.footprint_bytes;
  options.verification.requested_isa = options_.isa;
  options.verification.lock = options_.lock;
  options.verification.isa_probe = isa_probe_.get();
  options.verification.duty = candidate.duty;
  options.verification.line_bytes = line_bytes_of(*machine_);
  options.verification.cpus = candidate.cpus.empty() ? allowed_cpus_ : candidate.cpus;
  std::ranges::sort(options.verification.cpus);
  const auto repeated = std::ranges::unique(options.verification.cpus);
  options.verification.cpus.erase(repeated.begin(), repeated.end());
  options.sample_interval_ns = options_.sample_interval_ns;
  fpenv::RealFpControl fp;
  options.fp = &fp;

  const std::int64_t settle_interval_ns =
      candidate.settle.has_value()
          ? std::clamp(candidate.settle->window_ns / 4, kSettleFloorNs,
                       std::max(kSettleFloorNs, options_.sample_interval_ns))
          : kSettleFloorNs;
  auto made = make_rig(
      *syscalls_, files_, options_.roots,
      capacity_for(candidate, options, machine_->cpus.logical_cpu_count(), settle_interval_ns),
      options_.sources);
  if (!made.has_value()) {
    std::string detail = "cannot set up telemetry: ";
    detail += telemetry::describe(made.error().fault);
    detail += ": ";
    detail += made.error().detail;
    return refuse(TrialFault::kRun, std::move(detail));
  }
  const std::unique_ptr<Rig> rig = std::move(made).value();
  options.sampler = &rig->sampler();
  options.timeline = &rig->timeline();

  const statistics::Summarizer& summarizer =
      options_.summarizer != nullptr ? *options_.summarizer : real_summarizer_;

  TrialOutcome outcome;
  outcome.candidate = candidate.id;
  if (candidate.settle.has_value()) {
    auto settled = settle(*rig, summarizer, *candidate.settle, settle_interval_ns);
    if (!settled.has_value()) {
      return std::move(settled).error();
    }
    outcome.settled = settled.value().met;
    outcome.settle_ns = settled.value().ns;
  }

  reporters::ConsoleReporter reporter(log);
  controller::Controller supervisor(*syscalls_, reporter);
  log.flush();
  auto ran = supervisor.run(config, options);
  if (!ran.has_value()) {
    return refuse(TrialFault::kRun, describe(ran.error()));
  }
  const controller::RunReport report = std::move(ran).value();
  if (report.worker_exit_code.has_value()) {
    worker_exit_code_ = report.worker_exit_code;
    return refuse(TrialFault::kRun,
                  "this process is a worker whose exec failed: it must exit with " +
                      std::to_string(*report.worker_exit_code));
  }
  if (report.sampling.fault.has_value()) {
    return refuse(TrialFault::kRun,
                  "telemetry stopped during the trial: " + *report.sampling.fault);
  }

  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.capabilities = rig->capabilities();
  inputs.timeline = &rig->timeline();
  inputs.summarizer = &summarizer;
  auto result = assemble_result(inputs);
  if (!result.has_value()) {
    return refuse(TrialFault::kRun, "cannot compute the trial's result: " + result.error());
  }
  outcome.verdict = result.value().verdict;

  const std::int64_t end_ns = deadline_of(rig->timeline(), report) + statistics::kClosingMargin;
  auto signature = summarizer.summarize(rig->timeline(),
                                        statistics::Window{end_ns - candidate.duration_ns, end_ns});
  if (!signature.has_value()) {
    return refuse(TrialFault::kRun,
                  "cannot summarise the load window: " + describe_statistics(signature.error()));
  }
  outcome.signature = std::move(signature).value();
  log << "trial " << candidate.id << ": " << results::describe(outcome.verdict)
      << (outcome.settled ? ", settled" : ", not settled") << " after " << outcome.settle_ns
      << " ns\n";
  return outcome;
}

core::Result<ControllerTrialRunner::Settled, TrialError> ControllerTrialRunner::settle(
    Rig& rig, const statistics::Summarizer& summarizer, const Settle& phase,
    std::int64_t interval_ns) {
  platform::Clock clock(*syscalls_);
  const auto begun = clock.now();
  if (!begun.has_value()) {
    return refuse(TrialFault::kRun, "settle: " + platform::describe(begun.error()));
  }
  const std::int64_t started_ns = begun.value().running_ns;
  for (;;) {
    const auto now = clock.now();
    if (!now.has_value()) {
      return refuse(TrialFault::kRun, "settle: " + platform::describe(now.error()));
    }
    const std::int64_t now_ns = now.value().running_ns;
    const std::int64_t elapsed_ns = now_ns - started_ns;
    if (auto taken = rig.sampler().round(now_ns); !taken.has_value()) {
      std::string detail = "settle: sampling refused: ";
      detail += telemetry::describe(taken.error().fault);
      return refuse(TrialFault::kRun, std::move(detail));
    }
    if (elapsed_ns >= phase.window_ns) {
      const auto slope =
          slope_of(summarizer, rig.timeline(),
                   statistics::Window{now_ns - phase.window_ns, now_ns + 1}, phase.metric);
      if (slope.has_value() && std::abs(*slope) <= phase.max_slope_per_second) {
        return Settled{true, std::min(elapsed_ns, phase.max_ns)};
      }
    }
    if (elapsed_ns >= phase.max_ns) {
      return Settled{false, phase.max_ns};
    }
    const std::int64_t wait_ns = std::min(interval_ns, phase.max_ns - elapsed_ns);
    // Rounded up, so that a wait never returns before its instant.
    const auto wait_ms =
        static_cast<int>((wait_ns + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond);
    const auto slept = syscalls_->poll_readable({}, wait_ms);
    if (!slept.has_value() && slept.error().number != EINTR) {
      return refuse(TrialFault::kRun, "settle: " + platform::describe(slept.error()));
    }
  }
}

}  // namespace loadforge::cli
