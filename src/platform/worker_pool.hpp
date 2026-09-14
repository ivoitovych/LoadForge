// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_WORKER_POOL_HPP
#define LOADFORGE_PLATFORM_WORKER_POOL_HPP

#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/exit_status.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// A worker that stopped running: which slot, which pid, and how.
struct WorkerDeath {
  std::size_t slot = 0;  ///< Index in the argv list the pool was spawned from.
  pid_t pid = 0;
  ExitStatus status{0};

  [[nodiscard]] friend bool operator==(const WorkerDeath& lhs, const WorkerDeath& rhs) {
    return lhs.slot == rhs.slot && lhs.pid == rhs.pid && lhs.status == rhs.status;
  }
};

/// After a spawn, this process is either the supervisor or one of the workers.
///
/// fork(2) returns in both, so a loop that forks N times returns in N+1
/// processes. Every child must leave that loop immediately -- a child that kept
/// iterating would fork grandchildren, and the process tree would double with
/// every turn. Making the caller ask which side it is on is what prevents that;
/// a bare vector of pids would let a child fall through and keep going.
class SpawnOutcome {
 public:
  [[nodiscard]] static SpawnOutcome supervisor(std::vector<pid_t> pids) {
    return SpawnOutcome{std::move(pids), kNotAChild};
  }
  [[nodiscard]] static SpawnOutcome worker(std::size_t slot) { return SpawnOutcome{{}, slot}; }

  [[nodiscard]] bool is_worker() const noexcept { return slot_ != kNotAChild; }

  /// Which worker this process is. Valid only when is_worker().
  [[nodiscard]] std::size_t slot() const noexcept { return slot_; }

  /// The pids created. Valid only in the supervisor; empty in a worker, which
  /// created none.
  [[nodiscard]] const std::vector<pid_t>& pids() const noexcept { return pids_; }

 private:
  static constexpr std::size_t kNotAChild = static_cast<std::size_t>(-1);
  SpawnOutcome(std::vector<pid_t> pids, std::size_t slot) : pids_(std::move(pids)), slot_(slot) {}

  std::vector<pid_t> pids_;
  std::size_t slot_;
};

/// A set of worker processes, and the supervisor's half of their lifecycle.
///
/// WHY N >= 2 IS A TESTING RULE, NOT A CONFIGURATION DETAIL
/// -------------------------------------------------------
/// With a single worker, "the supervisor noticed a death" and "the supervisor
/// noticed the only child exited" are the same observation, so a passing test
/// proves nothing about supervision. Every test here runs at least two workers
/// and asserts which of them died (docs/PLAN.md §4.6).
///
/// WHAT THIS DELIBERATELY DOES NOT DO
/// ----------------------------------
/// It does not restart anything. A worker that dies is an EVENT THIS TOOL
/// EXISTS TO REPORT, not a fault to paper over: restarting it silently would
/// destroy the one observation the run was for. Restart policy, if it ever
/// exists, belongs to the controller above, with the death recorded first.
class WorkerPool {
 public:
  /// Consecutive EINTRs tolerated while reaping, for the same reason
  /// FileSystem bounds its read loop: a signal storm that interrupts every
  /// waitpid would spin here forever, and a supervisor that never returns is a
  /// hung tool -- the one outcome worse than a reported failure.
  static constexpr int kMaxConsecutiveInterrupts = 128;

  WorkerPool(Syscalls& syscalls, WorkerLauncher& launcher)
      : syscalls_(&syscalls), launcher_(&launcher) {}

  /// Forks one worker per argv entry.
  ///
  /// RETURNS IN EVERY PROCESS IT CREATES. A caller that gets `is_worker()` must
  /// call WorkerLauncher::become_worker and then _exit; it must never return
  /// into the supervisor's control flow.
  ///
  /// ON PARTIAL FAILURE THE POOL UNWINDS ITSELF. If the fourth of eight forks
  /// fails, the three already running are signalled and reaped before the error
  /// is returned, so a failed spawn leaves no workers behind. Returning the
  /// error without that cleanup would strand processes that no longer have a
  /// supervisor expecting them -- exactly the orphan the death signal exists to
  /// prevent, arrived at through the front door.
  [[nodiscard]] core::Result<SpawnOutcome, SyscallError> spawn(
      const std::vector<std::vector<std::string>>& argv_per_worker);

  /// Waits for the next worker to stop running.
  ///
  /// Reaps whichever child terminated first and matches it against the pids
  /// this pool created. A child that is NOT one of ours -- forked by some
  /// library, inherited, anything -- is reaped and skipped rather than reported
  /// as a worker: attributing a stranger's death to a worker would be
  /// fabricated evidence, and this tool's whole value is that its evidence is
  /// real.
  ///
  /// Returns ECHILD when there is nothing left to wait for. That is the
  /// ordinary end of a run and the caller distinguishes it by the errno, not by
  /// an empty result that could also mean something went wrong.
  [[nodiscard]] core::Result<WorkerDeath, SyscallError> wait_for_next();

  /// Sends a signal to every worker still believed to be running.
  ///
  /// ESRCH on an individual worker is NOT an error here: it means that worker
  /// already died and has not been reaped yet, which is a race the supervisor
  /// cannot avoid and must not fail on. Any other errno is reported, and the
  /// first one wins -- but every remaining worker is still signalled, because
  /// giving up halfway would leave the rest running.
  [[nodiscard]] core::Result<core::Ok, SyscallError> signal_all(int signal);

  /// Workers created and not yet reaped.
  [[nodiscard]] std::size_t live_count() const noexcept { return live_; }

  /// The pid in a slot, or 0 once that worker has been reaped.
  [[nodiscard]] pid_t pid_at(std::size_t slot) const;

 private:
  /// Signals and reaps whatever a failed spawn had already started.
  void unwind_partial_spawn();

  Syscalls* syscalls_;
  WorkerLauncher* launcher_;
  std::vector<pid_t> pids_;  ///< Indexed by slot; zeroed as each is reaped.
  std::size_t live_ = 0;
};

}  // namespace loadforge::platform

#endif
