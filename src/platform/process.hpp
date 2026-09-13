// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_PROCESS_HPP
#define LOADFORGE_PLATFORM_PROCESS_HPP

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// Which side of a fork(2) this process is on.
///
/// fork returns 0 in the child and a pid in the parent, and `if (pid == 0)` is
/// one typo away from running the child's path in the parent -- which, in a
/// supervisor, means the supervisor execs itself away and the run ends with no
/// diagnosis. A type that has to be asked makes that mistake require effort.
class ForkOutcome {
 public:
  [[nodiscard]] static ForkOutcome child() noexcept { return ForkOutcome{0}; }
  [[nodiscard]] static ForkOutcome parent(pid_t child_pid) noexcept {
    return ForkOutcome{child_pid};
  }

  [[nodiscard]] bool is_child() const noexcept { return child_pid_ == 0; }

  /// The child's pid. Meaningful only in the parent; returns 0 in the child,
  /// which is not a pid and cannot be mistaken for one.
  [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }

 private:
  explicit ForkOutcome(pid_t pid) noexcept : child_pid_(pid) {}
  pid_t child_pid_;
};

/// Why a worker process gave up before it began doing work.
///
/// These are exit codes, so they must survive the 8-bit truncation exit(2)
/// applies and must not collide with anything a real worker might return.
///
///   * Not 0-2: those are success and ordinary failure, and a worker that
///     failed for its own reasons must stay distinguishable from one that never
///     started.
///   * Not 126/127: the shell uses those for "found but not executable" and
///     "not found", and a user reading them would reasonably reach for the
///     shell's meaning.
///   * Not above 255: exit(256) reports as exit(0) (see ExitStatus), so a
///     distinguished failure code up there would announce itself as success.
enum class WorkerStartupFailure : std::uint8_t {
  kExecFailed = 120,              ///< execv returned, so the image was never replaced.
  kParentDiedBeforeArming = 121,  ///< Orphaned in the window fork..prctl.
  kDeathSignalUnavailable = 122,  ///< prctl(PR_SET_PDEATHSIG) failed.
};

/// Creates worker processes, and is the code that runs inside one before it
/// becomes a worker.
///
/// THE MODEL (docs/PLAN.md §4.6)
/// ----------------------------
/// Workers are separate processes, all of them this same executable started with
/// a different switch, created by fork(2) AND THEN exec(2). Not threads: a
/// thread that dies takes the supervisor's telemetry timeline with it, and the
/// seconds before a fault are the most valuable thing this tool produces. Not a
/// bare fork: a forked child inherits the parent's dirtied heap, its allocator
/// and sanitizer state, and any locks its threads held at fork time -- and this
/// is a tool that verifies memory, so starting from pages the supervisor already
/// touched is backwards. exec also re-randomises ASLR, so N workers occupy N
/// independent mappings.
///
/// WHY THE PARENT AND CHILD HALVES ARE SEPARATE FUNCTIONS
/// -----------------------------------------------------
/// A single spawn() would have to call _exit() in the child, and _exit cannot be
/// driven from a test: it takes the test process with it. Splitting the fork
/// from what the child does leaves every DECISION above the seam, where a fake
/// can force it, and reduces the untestable part to one line of glue in main()
/// that does nothing but call _exit with what become_worker returned. That glue
/// is covered by the integration tier, which runs the real binary.
class WorkerLauncher {
 public:
  /// The signal the kernel sends a worker when the supervisor dies.
  ///
  /// SIGKILL rather than SIGTERM: this is the last-resort guarantee against
  /// orphaned workers pinning every core on a machine whose owner has already
  /// lost the tool that was driving them. Graceful shutdown is the supervisor's
  /// job while it is alive; by the time this fires there is nobody left to be
  /// graceful with.
  static constexpr int kParentDeathSignal = 9;  // SIGKILL

  WorkerLauncher(Syscalls& syscalls, std::string executable_path)
      : syscalls_(&syscalls), executable_path_(std::move(executable_path)) {}

  /// fork(2), reported as a side rather than as a number.
  ///
  /// RETURNS IN BOTH PROCESSES, exactly as fork does. The caller must ask which
  /// side it is on and, in the child, call become_worker and then _exit with
  /// what it returns -- it must never return into the caller's own control flow,
  /// because the child would then run the supervisor's code with the
  /// supervisor's state.
  [[nodiscard]] core::Result<ForkOutcome, SyscallError> fork_worker();

  /// What the child does, up to the point where it stops being this program.
  ///
  /// Returns ONLY on failure, and returns the exit code the child must use.
  /// On success it has exec'd and this call does not return at all.
  ///
  /// `supervisor_pid` is the pid read BEFORE the fork. It is compared against
  /// the current parent afterwards to close the PR_SET_PDEATHSIG race: the
  /// supervisor can die between fork and prctl, and the death signal then never
  /// arrives, because the death it was armed for has already happened. Without
  /// this check the worker runs on with no supervisor -- the exact orphan the
  /// signal exists to prevent.
  [[nodiscard]] WorkerStartupFailure become_worker(pid_t supervisor_pid,
                                                   const std::vector<std::string>& argv);

  /// The error from the most recent failed exec, for the supervisor's report.
  ///
  /// Kept because become_worker can only return an exit code -- one byte, with
  /// no room for an errno and a path. In the child this is read before _exit;
  /// across the process boundary the supervisor sees only the code, and the
  /// detail arrives over the error-record pipe at M3.
  [[nodiscard]] const SyscallError& last_exec_error() const noexcept { return last_exec_error_; }

 private:
  Syscalls* syscalls_;
  std::string executable_path_;
  SyscallError last_exec_error_{};
};

/// Resolves the running executable's own path.
///
/// From /proc/self/exe and NEVER from argv[0]: argv[0] can be relative, the
/// caller can set it to anything, and the working directory can move before a
/// worker is spawned. /proc/self/exe is what the kernel says was executed.
///
/// Resolve it once at startup, before any chdir, and keep the result: the link
/// follows the file, so it stays correct after an upgrade replaces the binary on
/// disk, but only if it was read while the process was still the thing it names.
class ExecutablePath {
 public:
  /// Longest path this will accept. PATH_MAX on Linux is 4096 including the
  /// terminator; a link longer than this is refused rather than truncated,
  /// because a truncated path is a *plausible* path to the wrong file -- the
  /// same reasoning that makes FileSystem refuse an oversized read.
  static constexpr std::size_t kMaxPathLength = 4096;

  explicit ExecutablePath(Syscalls& syscalls) : syscalls_(&syscalls) {}

  /// The absolute path of the running executable, or why it could not be had.
  ///
  /// There is deliberately NO fallback to argv[0] when /proc is not mounted --
  /// a live medium without /proc is exactly where a wrong worker binary would
  /// be hardest to notice. The failure is reported so the supervisor can refuse
  /// multi-process mode and say why; an explicit override belongs on the
  /// command line, where the user can see what they chose.
  [[nodiscard]] core::Result<std::string, SyscallError> resolve(
      const std::string& link = "/proc/self/exe");

 private:
  Syscalls* syscalls_;
};

}  // namespace loadforge::platform

#endif
