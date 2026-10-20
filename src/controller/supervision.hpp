// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_SUPERVISION_HPP
#define LOADFORGE_CONTROLLER_SUPERVISION_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/exit_status.hpp"

namespace loadforge::controller {

/// What became of a worker, as the controller will report it.
///
/// This is the vocabulary of IMPLEMENTATION §4's state machine and
/// misbehaviour catalogue, and the distinctions are the point: a worker
/// that exited 1 is a worker error, one killed by SIGBUS is a process fault,
/// and one the controller itself stopped is neither.
///
///   kCompleted              exited 0 with its finished record delivered.
///   kWorkerError            exited non-zero for its own reasons (including
///                           119, its own setup failure).
///   kStartupFailed          never did any work: the launcher's 120-123, or
///                           125, a worker that refused its arguments.
///   kControllerLostReported exited 124: the WORKER believed the controller
///                           was gone. If the controller is reporting this, it
///                           was not gone, and the fault is the controller's.
///   kFault                  killed by a fault signal: SIGSEGV, SIGBUS,
///                           SIGABRT, SIGILL, SIGFPE, SIGTRAP. A process
///                           fault that may have software or hardware causes:
///                           abort() and a read past a truncated mapping
///                           raise SIGABRT and SIGBUS as surely as a failing
///                           memory module does. The signal establishes that
///                           the process died, never why; a verdict on the
///                           hardware belongs to verification and isolation
///                           (docs/verification.md §7a), not to the signal.
///   kKilled                 killed by a signal with nothing to say who sent
///                           it: one the controller did not send, or a SIGKILL
///                           it attempted that the kernel's OOM-kill count
///                           could neither confirm nor rule out as another's
///                           (provenance ambiguous). A SIGKILL from the OOM
///                           killer looks exactly like this until the
///                           kernel's own record says otherwise.
///   kOomKilled              killed by a SIGKILL, and the kernel's OOM-kill
///                           count accounts for it: the memory a run asked
///                           for, not the hardware. This outranks a SIGKILL
///                           the controller attempted, which proves nothing
///                           about the cause.
///   kStoppedByController    killed by the signal the controller sent after
///                           asking nicely at the deadline.
///   kTimedOut               killed by the controller because its heartbeat
///                           went stale: the hang and the spin.
///   kChannelCorrupt         its record pipe carried a frame that did not
///                           verify. Reported over everything else: garbage
///                           in must never become a verdict out.
///   kTruncatedEvidence      a clean exit with no finished record, or a
///                           stream that ended inside a frame.
///   kUnrecognised           a status word the decoder could not classify.
enum class WorkerFate : std::uint8_t {
  kCompleted,
  kWorkerError,
  kStartupFailed,
  kControllerLostReported,
  kFault,
  kKilled,
  kOomKilled,
  kStoppedByController,
  kTimedOut,
  kChannelCorrupt,
  kTruncatedEvidence,
  kUnrecognised,
};

[[nodiscard]] std::string_view describe(WorkerFate fate) noexcept;

/// The one fate a run may count as clean.
[[nodiscard]] constexpr bool is_clean(WorkerFate fate) noexcept {
  return fate == WorkerFate::kCompleted;
}

/// Whether a termination signal is one the kernel delivers for a fault in
/// the program, as opposed to one sent by somebody.
[[nodiscard]] bool is_fault_signal(int signal) noexcept;

/// Everything the controller knows about a worker once it is reaped.
struct Evidence {
  platform::ExitStatus status{0};
  /// The last signal the controller sent it, while a look at the worker's exit
  /// status, just before, showed it still running. An ATTEMPT, and nothing
  /// more: kill(2) succeeds on a zombie, so a delivered signal does not prove
  /// that it, and not another SIGKILL, ended the worker (journal §1.27). A
  /// worker already found exited is never signalled and never recorded here.
  std::optional<int> signal_attempted;
  bool stale_when_signalled = false;  ///< That signal was for a stale heartbeat.
  bool finished_seen = false;         ///< Its finished record arrived.
  bool channel_corrupt = false;       ///< A frame failed to verify.
  bool channel_truncated = false;     ///< The stream ended inside a frame.
  /// The kernel's OOM-kill count covers this worker's death. Set only for an
  /// oom_candidate, and only by judge_oom_kills's verdict.
  bool oom_kill_recorded = false;
  /// The kernel's OOM-kill count was consulted and could neither account for
  /// this worker's death nor rule an OOM kill out: it could not be read, it
  /// went down, or it rose by fewer than the deaths it would have to explain.
  /// Set, with an attempted SIGKILL that matches a SIGKILL death, it is the
  /// ambiguous provenance classify reports as kKilled.
  bool oom_inconclusive = false;

  [[nodiscard]] friend bool operator==(const Evidence&, const Evidence&) = default;
};

/// The classification. Pure, so the table is tested directly: every row of
/// the misbehaviour catalogue is one Evidence and one expected fate.
[[nodiscard]] WorkerFate classify(const Evidence& evidence) noexcept;

/// Whether a worker died the way the OOM killer kills: by a SIGKILL, on a
/// channel that did not go corrupt. The only deaths the kernel's OOM-kill
/// count can explain; every other fate already has its cause. A SIGKILL the
/// controller attempted is still a candidate: the attempt may have met a
/// worker already dead of another SIGKILL (journal §1.27), so the count is
/// always consulted.
[[nodiscard]] bool oom_candidate(const Evidence& evidence) noexcept;

/// What the kernel's OOM-kill count says about the run's oom_candidates, and
/// the line each candidate's report carries either way.
struct OomFinding {
  bool attributed = false;
  std::string note;
  /// The count was read at both ends and did not rise: no OOM kill happened
  /// during the run, so none can have ended a worker. Neither attributed nor
  /// ruled out is inconclusive.
  bool ruled_out = false;
};

/// Judges the count read before the first fork and after the last reap,
/// against how many workers are candidates (at least one).
///
/// THE RULE, AND WHY IT IS THIS ONE
/// --------------------------------
/// The count says how many processes in the cgroup the OOM killer killed, and
/// not which. So the candidates are attributed together or not at all: all
/// of them when the count rose by at least as many, none when it rose by
/// fewer. Two workers dead of SIGKILL and one OOM kill counted means one was
/// the OOM killer and one was somebody's `kill -9`, and nothing in the record
/// says which; calling both OOM kills, or picking one, would report an
/// inference as a finding.
///
/// The window is the whole run and not each death, because the kernel counts
/// a kill when it chooses the victim and the victim can be reaped a tick
/// later: a worker killed with `kill -9` and reaped in the tick between
/// another worker's OOM kill and that worker's reap would otherwise take the
/// other's count and swap the two verdicts. Over the whole run the count and
/// the deaths are complete.
///
/// What no count can rule out is a process outside the run but inside the
/// cgroup being OOM-killed while a worker died of somebody's SIGKILL; the
/// narrower the cgroup, the smaller that window. A count that went down, or
/// that could not be read at either end, attributes nothing.
[[nodiscard]] OomFinding judge_oom_kills(const core::Result<std::uint64_t, std::string>& before,
                                         const core::Result<std::uint64_t, std::string>& after,
                                         std::size_t candidates, std::string_view scope);

}  // namespace loadforge::controller

#endif
