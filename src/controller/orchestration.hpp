// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_ORCHESTRATION_HPP
#define LOADFORGE_CONTROLLER_ORCHESTRATION_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/safety.hpp"
#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "resources/oom_kill_record.hpp"

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
  /// first fork and, when a worker died of a SIGKILL the controller did not
  /// send, after the last reap (judge_oom_kills has the rule). Null when none
  /// is consulted: such a death is then reported as a kill, which is the fact.
  /// Not owned; it must outlive the run.
  resources::OomKillRecord* oom_record = nullptr;

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
/// WHAT THE CONTROLLER DOES BEFORE IT FORKS
/// ----------------------------------------
/// It publishes its first beat. A worker gives a controller that has never
/// beaten exactly the staleness allowance and then leaves, so the beat must
/// exist before the first worker can look. And it forks before any thread
/// starts (docs/PLAN.md §4.6): at M1 there are none; the rule is stated here
/// for the milestone that adds them.
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
