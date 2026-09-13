// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/process.hpp"

#include <cerrno>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {

core::Result<ForkOutcome, SyscallError> WorkerLauncher::fork_worker() {
  auto forked = syscalls_->fork_process();
  if (!forked) {
    return forked.error();
  }
  return forked.value() == 0 ? ForkOutcome::child() : ForkOutcome::parent(forked.value());
}

WorkerStartupFailure WorkerLauncher::become_worker(pid_t supervisor_pid,
                                                   const std::vector<std::string>& argv) {
  // Arm the death signal first, so the window in which this child can be
  // orphaned unnoticed is as short as the kernel allows.
  if (auto armed = syscalls_->set_parent_death_signal(kParentDeathSignal); !armed) {
    return WorkerStartupFailure::kDeathSignalUnavailable;
  }

  // THE RACE, closed. PR_SET_PDEATHSIG fires when the parent dies -- but if the
  // parent already died, between the fork and the line above, there is no death
  // left to signal and the worker would run on unsupervised forever. getppid()
  // is the only way to see it: an orphan has been reparented, so its parent pid
  // no longer matches the supervisor that forked it.
  //
  // Comparing against the pid captured BEFORE the fork matters. Reading it here
  // and comparing it to itself would be vacuous, and comparing against 1 assumes
  // init is the reaper -- which is wrong under a subreaper, in a container, or
  // under any process that called PR_SET_CHILD_SUBREAPER.
  if (syscalls_->parent_pid() != supervisor_pid) {
    return WorkerStartupFailure::kParentDiedBeforeArming;
  }

  // On success this does not return: the image is replaced and everything below
  // belongs to a different program.
  last_exec_error_ = syscalls_->exec(executable_path_, argv);
  return WorkerStartupFailure::kExecFailed;
}

core::Result<std::string, SyscallError> ExecutablePath::resolve(const std::string& link) {
  std::string buffer(kMaxPathLength, '\0');
  auto count = syscalls_->read_link(link, buffer.data(), buffer.size());
  if (!count) {
    return count.error();
  }

  // readlink does not null-terminate and does not report truncation: a target
  // longer than the buffer fills it completely and returns the buffer size,
  // which is indistinguishable from an exact fit. Treating a full buffer as
  // truncation costs us the one path that is exactly kMaxPathLength bytes long
  // and buys certainty that no silently shortened path is ever returned. A
  // truncated path is not a broken string, it is a valid path to the wrong file.
  if (count.value() >= buffer.size()) {
    return SyscallError{ENAMETOOLONG, "readlink", link};
  }

  // A zero-length link is not something Linux produces for /proc/self/exe, but
  // an empty string would be accepted downstream as a path and fail much later
  // with no clue where it came from (F20: refuse what cannot be interpreted).
  if (count.value() == 0) {
    return SyscallError{ENOENT, "readlink", link + " (resolved to an empty path)"};
  }

  buffer.resize(count.value());
  return buffer;
}

}  // namespace loadforge::platform
