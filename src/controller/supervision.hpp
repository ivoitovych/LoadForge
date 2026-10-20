// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_SUPERVISION_HPP
#define LOADFORGE_CONTROLLER_SUPERVISION_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "platform/exit_status.hpp"

namespace loadforge::controller {

/// What became of a worker, as the controller will report it.
///
/// This is the vocabulary of IMPLEMENTATION §4's state machine and
/// misbehaviour catalogue, and the distinctions are the point: a worker
/// that exited 1 is a worker error, one killed by SIGBUS is a hardware
/// event, and one the controller itself stopped is neither.
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
///                           SIGABRT, SIGILL, SIGFPE, SIGTRAP.
///   kKilled                 killed by a signal the controller did not send.
///                           SIGKILL from the OOM killer looks exactly like
///                           this; the kernel's own record, when it is read,
///                           upgrades it to a distinct classification.
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
  std::optional<int> signal_sent;     ///< The last signal the controller sent it.
  bool stale_when_signalled = false;  ///< That signal was for a stale heartbeat.
  bool finished_seen = false;         ///< Its finished record arrived.
  bool channel_corrupt = false;       ///< A frame failed to verify.
  bool channel_truncated = false;     ///< The stream ended inside a frame.

  [[nodiscard]] friend bool operator==(const Evidence&, const Evidence&) = default;
};

/// The classification. Pure, so the table is tested directly: every row of
/// the misbehaviour catalogue is one Evidence and one expected fate.
[[nodiscard]] WorkerFate classify(const Evidence& evidence) noexcept;

}  // namespace loadforge::controller

#endif
