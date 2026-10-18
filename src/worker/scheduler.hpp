// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_SCHEDULER_HPP
#define LOADFORGE_WORKER_SCHEDULER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/clock.hpp"
#include "platform/syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"

namespace loadforge::worker {

/// How a worker paces itself. All three come from the controller on the
/// command line, so both sides argue from the same numbers.
struct Pacing {
  std::int64_t duration_ns = 0;     ///< Stop, complete, after this much raw time.
  std::int64_t interval_ns = 0;     ///< Beat, and check on the controller, this often.
  std::int64_t stale_after_ns = 0;  ///< The controller is lost once its beat is older.

  [[nodiscard]] friend bool operator==(const Pacing&, const Pacing&) = default;
};

/// Why the loop stopped.
///
///   kDurationElapsed      the work is done.
///   kStopRequested        the controller wrote on the control pipe: stop now,
///                         cleanly. How a run ends early on request.
///   kControllerHungUp     the control pipe's writer is gone: the controller
///                         exited, crashed or was killed. The worker must not
///                         outlive it (docs/PLAN.md F9).
///   kControllerStale      the controller's beat is older than allowed, or
///                         never came within that allowance. A controller that
///                         is alive and wedged holds its pipes open, so this is
///                         the only way to see it.
///   kControllerUnreadable the controller's slot could not be read: it died
///                         inside a publish.
enum class StopReason : std::uint8_t {
  kDurationElapsed,
  kStopRequested,
  kControllerHungUp,
  kControllerStale,
  kControllerUnreadable,
};

[[nodiscard]] std::string_view describe(StopReason reason) noexcept;

/// The reasons that mean the worker stopped because the controller did.
[[nodiscard]] bool controller_lost(StopReason reason) noexcept;

struct Outcome {
  StopReason reason = StopReason::kDurationElapsed;
  std::uint64_t progress = 0;   ///< Ticks completed: the null workload's unit of work.
  std::int64_t elapsed_ns = 0;  ///< Raw time from the first beat to the decision.

  [[nodiscard]] friend bool operator==(const Outcome&, const Outcome&) = default;
};

/// Something the worker could not do, and at which step.
struct WorkerError {
  std::string stage;   ///< "clock", "heartbeat", "control pipe".
  std::string detail;  ///< The underlying error, rendered.

  [[nodiscard]] friend bool operator==(const WorkerError&, const WorkerError&) = default;
};

[[nodiscard]] std::string describe(const WorkerError& error);

/// What a worker is attached to.
struct Session {
  ipc::HeartbeatRegion* region = nullptr;
  std::size_t slot = 0;  ///< This worker's index; it beats in worker_slot(slot).
  int control_fd = -1;   ///< Read end of the controller's control pipe.
  Pacing pacing;
};

/// The null workload: a loop that does no work and does everything else a
/// workload must -- beat, watch the controller, stop on request, stop on
/// time. M1 exit criterion 1 (docs/IMPLEMENTATION.md §4) is this loop under a
/// real controller; every later workload is this loop with work inside it.
///
/// THE ORDER OF ONE TICK, AND WHY
/// ------------------------------
/// Read the clock, publish the beat, then judge. The beat goes out before the
/// duration check so that a worker which is about to complete has still
/// reported itself alive, and before the controller check so that a worker
/// which is about to leave has left a last beat with its final progress. Then
/// one poll on the control pipe is the tick's wait: a byte on it is a stop
/// request, a hang-up is the controller gone, and the timeout is the interval.
class NullWorkload {
 public:
  static constexpr std::uint32_t kPhaseRunning = 1;

  NullWorkload(platform::Syscalls& syscalls, Session session) noexcept
      : syscalls_(&syscalls), session_(session) {}

  [[nodiscard]] core::Result<Outcome, WorkerError> run();

 private:
  /// The reason to leave that the controller's slot gives, if any: unreadable,
  /// stale, or never beat past the allowance.
  [[nodiscard]] std::optional<StopReason> controller_gone(const platform::TimePoint& now,
                                                          std::int64_t elapsed_ns) const;

  /// One interval spent watching the control pipe: a reason to stop, nothing
  /// (the interval passed), or a failure of the pipe itself.
  [[nodiscard]] core::Result<std::optional<StopReason>, WorkerError> wait_on_control(
      int interval_ms);

  platform::Syscalls* syscalls_;
  Session session_;
};

}  // namespace loadforge::worker

#endif
