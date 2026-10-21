// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_ORCHESTRATION_HPP
#define LOADFORGE_CONTROLLER_ORCHESTRATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/plan.hpp"
#include "controller/report.hpp"
#include "controller/safety.hpp"
#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscalls.hpp"
#include "resources/oom_kill_record.hpp"
#include "telemetry/sampler.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::controller {

/// What a run needs beyond its configuration.
struct RunOptions {
  std::string executable;  ///< The worker binary: this one, resolved from /proc/self/exe.
  std::int64_t interval_ns = 100'000'000;       ///< Heartbeat and poll interval.
  std::int64_t stale_after_ns = 2'000'000'000;  ///< A beat older than this is a hang.
  Grace grace{1'000'000'000, 1'000'000'000};
  /// What the machine offers, for check_memory and the banner. None when it
  /// could not be read, and then `budget_unread` says why, in the reader's
  /// own words, so the banner's NOT ENFORCED carries its reason.
  std::optional<config::MemoryBudget> budget;
  std::string budget_unread;
  /// The kernel's OOM-kill count for the workers' cgroup, read before the
  /// first fork and, when a worker died of a SIGKILL -- one the controller
  /// attempted too, since an attempt proves nothing about the cause
  /// (journal §1.27) -- after the last reap (judge_oom_kills has the rule).
  /// Null when none is consulted: such a death is then reported as a kill,
  /// or as the controller's stop where it attempted the SIGKILL.
  /// Not owned; it must outlive the run.
  resources::OomKillRecord* oom_record = nullptr;
  /// The machine, described once before the first fork (docs/PLAN.md §4.1;
  /// the transition out of START in IMPLEMENTATION §4 M1). The caller takes
  /// it, as it reads the budget and opens the record, so that it exists
  /// before the controller can fork; `loadforge stress` refuses to start
  /// without one. Null when none is offered, which the plan then says. Not
  /// owned; it must outlive the run.
  const topology::Snapshot* topology = nullptr;
  /// The workloads that verify themselves (plan.hpp). Without a registry a
  /// configured name is a label for the null workload, as at M1.
  VerificationOptions verification;
  /// The floating-point control the run establishes and asserts at startup
  /// (docs/determinism.md §3) and states in the banner; none: not asserted.
  /// Not owned; it must outlive the run.
  fpenv::FpControl* fp = nullptr;
  /// The telemetry sources probed once before the first fork, whose
  /// capability report the banner states (M2a exit criterion 5). Not owned.
  std::vector<telemetry::Source*> sources;
  /// The telemetry sampler (telemetry/sampler.hpp), already probed by the
  /// caller, which states the capability report before the run and keeps it
  /// for the result. The controller calls round() on its own tick, so sampling
  /// adds no thread (the sampler's contract): once before the first fork, so
  /// that a counter has its baseline before any worker can move it; then every
  /// `sample_interval_ns`; once at the deadline, on the boundary of the phase
  /// it ends; and once after the last reap. A round the timeline refuses ends
  /// sampling and is reported; it never ends the run. Not owned. Null: no
  /// telemetry.
  telemetry::Sampler* sampler = nullptr;
  /// The timeline the sampler writes, for the phase marks (F13: a counter is
  /// reported per phase). Marks "run" at the first round and "stop" at the
  /// deadline. Not owned; may be null even with a sampler.
  telemetry::TimelineSink* timeline = nullptr;
  /// The interval between rounds. A tick comes at least this often (the poll
  /// interval above), so a value below `interval_ns` samples at that rate.
  std::int64_t sample_interval_ns = 100'000'000;

  [[nodiscard]] friend bool operator==(const RunOptions&, const RunOptions&) = default;
};

/// Something the controller itself could not do, and at which step. A worker
/// misbehaving is never this: that is a WorkerFate in the report.
struct ControllerError {
  std::string stage;
  std::string detail;

  [[nodiscard]] friend bool operator==(const ControllerError&, const ControllerError&) = default;
};

[[nodiscard]] std::string describe(const ControllerError& error);

/// The controller: M1 exit criterion 1 -- runs the null workload in N worker
/// processes for the configured duration, honouring its limits, and reports
/// what became of each.
///
/// ONE LOOP, ONE BLOCKING CALL
/// ---------------------------
/// Each tick: read the clock, publish the controller's own beat (slot 0, the
/// one every worker watches), request a stop if the deadline has passed,
/// judge each live worker's beat and escalate where due, then poll the
/// record pipes for one interval -- the only call that waits -- drain what
/// arrived, and reap without blocking. A worker's exit closes its record
/// pipe, which wakes the poll, so a death is seen within one tick.
///
/// WHAT THE CONTROLLER DOES BEFORE IT FORKS, AND BETWEEN FORKS
/// -----------------------------------------------------------
/// It publishes its first beat. A worker gives a controller that has never
/// beaten exactly the staleness allowance and then leaves, so the beat must
/// exist before the first worker can look. And it forks before any thread
/// starts (docs/PLAN.md §4.6): at M1 there are none; the rule is stated here
/// for the milestone that adds them.
///
/// It beats again after every fork, in the spawn loop, because workers that
/// are already running watch slot 0 while the rest are still being created,
/// and a spawn can take longer than the staleness allowance (journal §2.33).
/// Each worker's allowance for a first heartbeat, and the clock for its lack
/// of progress, run from its own launch.
///
/// WHEN THE CONFIGURED DURATION STARTS
/// -----------------------------------
/// `run.duration` is the time the workers run, counted from the moment the
/// last worker has been launched. Starting them is not part of it. Each
/// worker also paces itself by the same duration from its own start, so every
/// worker runs the full duration and the whole run lasts the time it took to
/// start them, plus the duration.
class Controller {
 public:
  Controller(platform::Syscalls& syscalls, Reporter& reporter) noexcept
      : syscalls_(&syscalls), reporter_(&reporter) {}

  /// RETURNS IN THE CHILD TOO when a worker's exec fails: the report then
  /// carries `worker_exit_code`, and the caller must exit with it.
  [[nodiscard]] core::Result<RunReport, ControllerError> run(const config::Config& config,
                                                             const RunOptions& options);

 private:
  platform::Syscalls* syscalls_;
  Reporter* reporter_;
};

}  // namespace loadforge::controller

#endif
