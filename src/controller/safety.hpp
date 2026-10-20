// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_SAFETY_HPP
#define LOADFORGE_CONTROLLER_SAFETY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/result.hpp"

namespace loadforge::controller {

/// How far along the controller is in stopping one worker.
///
///   kNone        running; nothing asked of it.
///   kRequested   a stop byte was written on its control pipe: the graceful
///                request, which a working worker answers by finishing.
///   kTerminated  SIGTERM was sent, after the request went unanswered, or at
///                once for a worker whose heartbeat went stale.
///   kKilled      SIGKILL was sent. Nothing comes after it.
enum class StopStep : std::uint8_t { kNone, kRequested, kTerminated, kKilled };

[[nodiscard]] std::string_view describe(StopStep step) noexcept;

/// How long each step is given before the next: the request before SIGTERM,
/// SIGTERM before SIGKILL. Bounded, as IMPLEMENTATION §4 requires of a worker
/// that ignores SIGTERM: escalation ends in a signal that cannot be ignored.
struct Grace {
  std::int64_t stop_ns = 0;  ///< After the request, before SIGTERM.
  std::int64_t kill_ns = 0;  ///< After SIGTERM, before SIGKILL.

  [[nodiscard]] friend bool operator==(const Grace&, const Grace&) = default;
};

/// The signal due now for a worker at `step` since `since_ns`, if any: a
/// pure function of time, so the escalation ladder is tested without a
/// process. Strict: the grace must have fully elapsed.
[[nodiscard]] std::optional<int> escalation_due(StopStep step, std::int64_t since_ns,
                                                std::int64_t now_ns, const Grace& grace) noexcept;

/// The step a worker is at once `signal` has been sent to it.
[[nodiscard]] StopStep step_after(int signal) noexcept;

/// One configured limit and whether this build enforces it. A limit the tool
/// cannot enforce is reported as such at the start of the run, never
/// silently ignored: a user who configured a thermal ceiling must learn
/// before the run that nothing is watching it (docs/PLAN.md F3).
struct LimitStatus {
  std::string name;
  bool enforced = false;
  std::string note;

  [[nodiscard]] friend bool operator==(const LimitStatus&, const LimitStatus&) = default;
};

/// The limits a run states, each with its enforcement. The duration is always
/// enforced (it is the controller's deadline); memory is enforced against a
/// budget when one is known, and reported unenforced when none can be read;
/// temperature waits for telemetry.
[[nodiscard]] std::vector<LimitStatus> limit_statuses(
    const config::Config& config, const std::optional<config::MemoryBudget>& budget);

/// Refuses a run whose configured footprints exceed the budget, before any
/// worker starts. With no budget there is nothing to check against, and the
/// absence is reported through limit_statuses rather than treated as room.
[[nodiscard]] core::Result<core::Ok, std::string> check_memory(
    const config::Config& config, const std::optional<config::MemoryBudget>& budget);

}  // namespace loadforge::controller

#endif
