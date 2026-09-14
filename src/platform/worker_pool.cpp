// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/worker_pool.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/exit_status.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {

core::Result<SpawnOutcome, SyscallError> WorkerPool::spawn(
    const std::vector<std::vector<std::string>>& argv_per_worker) {
  pids_.assign(argv_per_worker.size(), 0);
  live_ = 0;

  for (std::size_t slot = 0; slot < argv_per_worker.size(); ++slot) {
    auto forked = launcher_->fork_worker();
    if (!forked) {
      // Unwind, then report THE CAUSE. The error is returned directly from
      // `forked` as the last thing this block does, rather than copied into a
      // local first: a copy taken before the cleanup would be a value nobody
      // needs, and a reference held across it would be one more thing to keep
      // valid if the cleanup ever grows.
      unwind_partial_spawn();
      return forked.error();
    }

    if (forked.value().is_child()) {
      // This process is a worker. It must leave the loop immediately -- another
      // turn would fork a grandchild and double the tree.
      return SpawnOutcome::worker(slot);
    }

    pids_[slot] = forked.value().child_pid();
    ++live_;
  }

  return SpawnOutcome::supervisor(pids_);
}

void WorkerPool::unwind_partial_spawn() {
  // Everything started so far is signalled and reaped, so a failed spawn leaves
  // nothing running. Both results are deliberately discarded: this path is
  // already reporting a failure, and a second one from the cleanup would
  // replace the cause with a consequence.
  (void)signal_all(SIGKILL);
  while (live_ > 0) {
    auto reaped = wait_for_next();
    if (!reaped) {
      break;  // ECHILD, or a kernel that will not tell us; stop rather than spin.
    }
  }
  pids_.clear();
}

core::Result<WorkerDeath, SyscallError> WorkerPool::wait_for_next() {
  int interrupts = 0;
  while (true) {
    auto reaped = syscalls_->wait_any();
    if (!reaped) {
      if (reaped.error().number == EINTR) {
        if (++interrupts < kMaxConsecutiveInterrupts) {
          continue;
        }
        return SyscallError{
            EINTR, "waitpid",
            "abandoned after " + std::to_string(interrupts) + " consecutive interruptions"};
      }
      return reaped.error();
    }
    // A successful reap clears the run: the bound is on consecutive
    // interruptions, not on how long a supervisor may wait in total.
    interrupts = 0;

    for (std::size_t slot = 0; slot < pids_.size(); ++slot) {
      if (pids_[slot] != 0 && pids_[slot] == reaped.value().pid) {
        pids_[slot] = 0;
        --live_;
        return WorkerDeath{slot, reaped.value().pid, ExitStatus{reaped.value().status}};
      }
    }
    // Not one of ours. It has been reaped -- waitpid already did that and it
    // cannot be undone -- but it is not reported as a worker death. Looping
    // rather than returning is the point: the caller asked for the next WORKER,
    // and handing it a stranger would be evidence this tool invented.
  }
}

core::Result<core::Ok, SyscallError> WorkerPool::signal_all(int signal) {
  bool failed = false;
  SyscallError first{};

  for (const pid_t pid : pids_) {
    if (pid == 0) {
      continue;  // Already reaped.
    }
    auto sent = syscalls_->send_signal(pid, signal);
    if (!sent && sent.error().number != ESRCH && !failed) {
      // ESRCH means that worker died between the last reap and this signal,
      // which is a race no supervisor can close and must not fail on.
      //
      // The first real error is kept and the loop continues: stopping here
      // would leave the remaining workers running, which is the opposite of
      // what a caller asking to signal them all wants.
      failed = true;
      first = sent.error();
    }
  }

  if (failed) {
    return first;
  }
  return core::Ok{};
}

pid_t WorkerPool::pid_at(std::size_t slot) const { return slot < pids_.size() ? pids_[slot] : 0; }

}  // namespace loadforge::platform
