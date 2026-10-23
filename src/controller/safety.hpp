// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_SAFETY_HPP
#define LOADFORGE_CONTROLLER_SAFETY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "controller/temperature.hpp"
#include "core/result.hpp"
#include "workloads/mixed/schedule.hpp"

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

/// One configured limit and whether this run enforces it. A limit the tool
/// cannot enforce is reported as such at the start of the run, never
/// silently ignored (docs/PLAN.md F3).
struct LimitStatus {
  std::string name;
  bool enforced = false;
  std::string note;
};

/// Every member alike. Defined once, in safety.cpp: a comparison defaulted in a
/// header is emitted by every test that compares what holds one, and the
/// coverage report cannot join the copies (docs/JOURNAL.md §1.22).
[[nodiscard]] bool operator==(const LimitStatus& lhs, const LimitStatus& rhs);

/// The limits a run states, each with its enforcement. The duration is always
/// enforced (it is the controller's deadline); memory is enforced against a
/// budget when one is known, and reported unenforced when none can be read,
/// with `unread` -- the reader's reason -- when it gave one. The temperature
/// ceiling is enforced when `watching` names the sensors that are watched (the
/// controller's TemperatureWatch::describe_sensors), and reported unenforced
/// when it is empty; a run does not get so far with a ceiling it cannot watch
/// (check_temperature refuses it), so that arm states a fact and not a policy.
[[nodiscard]] std::vector<LimitStatus> limit_statuses(
    const config::Config& config, const std::optional<config::MemoryBudget>& budget,
    std::string_view unread, std::string_view watching);

/// Refuses a run that configured a temperature ceiling and cannot keep it,
/// before any worker starts. `watch` is what the controller found to watch
/// after the baseline round, null when nothing could be (no sampler, no
/// timeline, a baseline round the timeline refused). Two refusals:
///
///   * no sensor answered in the baseline round. A ceiling the user asked for
///     and cannot get is the dangerous silent case (docs/PLAN.md F3,
///     docs/safety.md), so it is a refusal and not a banner line. `unread` is the
///     reason in the sources' own words, when there is one.
///   * a sensor is already at or above the ceiling. A run that would be stopped
///     at its first tick has nothing to stop.
///
/// Without a configured ceiling there is nothing to refuse.
[[nodiscard]] core::Result<core::Ok, std::string> check_temperature(const config::Config& config,
                                                                    const TemperatureWatch* watch,
                                                                    std::string_view unread);

/// Refuses a run whose configured footprints exceed the budget, before any
/// worker starts. With no budget there is nothing to check against, and the
/// absence is reported through limit_statuses rather than treated as room.
///
/// With a `schedule` the check holds for the whole of it: the largest total of
/// footprints any step leaves the workers holding (the starts, then every
/// Switch replacing its worker's footprint) is checked against the ceiling and
/// limits.memory, not only the start. A schedule is data made before the run,
/// so the whole check is made before the first fork. The schedule is one that
/// passed mixed::problem for the run's workers; a Switch naming a slot the run
/// does not have changes nothing. The brief overlap of an old runner and its
/// replacement is not counted (the worker releases before it prepares).
[[nodiscard]] core::Result<core::Ok, std::string> check_memory(
    const config::Config& config, const std::optional<config::MemoryBudget>& budget,
    const mixed::Schedule* schedule = nullptr);

}  // namespace loadforge::controller

#endif
