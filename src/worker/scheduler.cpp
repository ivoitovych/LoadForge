// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/scheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "platform/clock.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/control.hpp"
#include "worker/control_channel.hpp"
#include "worker/ipc/heartbeat.hpp"

namespace loadforge::worker {

int interval_milliseconds(std::int64_t interval_ns) noexcept {
  constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
  constexpr std::int64_t kLargest = std::numeric_limits<int>::max();
  return static_cast<int>(
      std::clamp<std::int64_t>(interval_ns / kNanosecondsPerMillisecond, 1, kLargest));
}

std::string_view describe(StopReason reason) noexcept {
  switch (reason) {
    case StopReason::kDurationElapsed:
      return "the duration elapsed";
    case StopReason::kStopRequested:
      return "the controller asked for a stop";
    case StopReason::kControllerHungUp:
      return "the controller's control pipe hung up";
    case StopReason::kControllerStale:
      return "the controller's heartbeat went stale";
    case StopReason::kControllerUnreadable:
      return "the controller's heartbeat could not be read";
    case StopReason::kControlChannelBroken:
      return "the controller's control pipe carried something that is not a message";
    case StopReason::kRunnerStopped:
      return "the verification runner stopped the run";
  }
  return "unrecognised stop reason";
}

bool controller_lost(StopReason reason) noexcept {
  return reason == StopReason::kControllerHungUp || reason == StopReason::kControllerStale ||
         reason == StopReason::kControllerUnreadable || reason == StopReason::kControlChannelBroken;
}

std::string describe(const WorkerError& error) { return error.stage + ": " + error.detail; }

namespace {

WorkerError error_at(std::string_view stage, std::string detail) {
  return WorkerError{std::string{stage}, std::move(detail)};
}

/// Why the null workload refuses a command: it runs no slices.
std::string nothing_to(const Command& command) {
  std::string text = "the null workload runs no slices: ";
  text += command_name(command);
  return text;
}

/// The null workload's answer to every message one look found, in order: a Stop is
/// answered Done and ends the run, anything else is refused, because there is
/// nothing here to pause, slow down, move or switch. The reason to leave, if the
/// messages gave one.
core::Result<std::optional<StopReason>, WorkerError> answer_all(
    ControlChannel& channel, const std::vector<ControlMessage>& messages) {
  for (const ControlMessage& message : messages) {
    const bool stop = std::holds_alternative<Stop>(message.command);
    std::string reason;
    if (!stop) {
      reason = nothing_to(message.command);
    }
    const Delivery sent =
        channel.answer(message.sequence, stop ? AckStatus::kDone : AckStatus::kRefused, reason);
    if (sent.failure.has_value()) {
      return *sent.failure;
    }
    if (sent.controller_gone) {
      return std::optional<StopReason>{StopReason::kControllerHungUp};
    }
    if (stop) {
      return std::optional<StopReason>{StopReason::kStopRequested};
    }
  }
  return std::optional<StopReason>{};
}
}  // namespace

std::optional<StopReason> ControllerWatch::gone(const platform::TimePoint& now,
                                                std::int64_t elapsed_ns) {
  // A reader that cannot get a consistent sample has a controller in the
  // middle of a publish: one that died there, or one the scheduler took off
  // the core between the two halves of its write, for as long as it likes.
  // One look cannot tell them apart, so the slot is unreadable only once it
  // has stayed so for longer than the allowance. A stale beat is a controller
  // that stopped beating; and one that never beat within the allowance is
  // treated as stale, because a controller publishes its first beat before it
  // spawns anyone.
  auto observed = session_->region->observe(ipc::kControllerSlot);
  if (!observed.has_value()) {
    if (now.running_ns - last_read_ns_ > session_->pacing.stale_after_ns) {
      return StopReason::kControllerUnreadable;
    }
    return std::nullopt;
  }
  last_read_ns_ = now.running_ns;
  const ipc::Liveness liveness =
      ipc::liveness(observed.value(), now.running_ns, session_->pacing.stale_after_ns);
  if (liveness == ipc::Liveness::kStale ||
      (liveness == ipc::Liveness::kNeverBeat && elapsed_ns > session_->pacing.stale_after_ns)) {
    return StopReason::kControllerStale;
  }
  return std::nullopt;
}

core::Result<Outcome, WorkerError> NullWorkload::run() {
  platform::Clock clock(*syscalls_);
  auto started = clock.now();
  if (!started.has_value()) {
    return error_at("clock", describe(started.error()));
  }
  const auto pid = static_cast<std::uint32_t>(syscalls_->process_id());
  const std::size_t slot = ipc::worker_slot(session_.slot);
  const int interval_ms = interval_milliseconds(session_.pacing.interval_ns);
  std::uint64_t progress = 0;
  ControllerWatch watch(session_, started.value().running_ns);
  ControlChannel channel(*syscalls_, session_.control_fd, *session_.records);

  while (true) {
    auto now = clock.now();
    if (!now.has_value()) {
      return error_at("clock", describe(now.error()));
    }
    auto elapsed = platform::Clock::between(started.value(), now.value());
    if (!elapsed.has_value()) {
      return error_at("clock", describe(elapsed.error()));
    }
    const std::int64_t elapsed_ns = elapsed.value().raw_ns;

    const ipc::Heartbeat beat{now.value().running_ns, progress, kPhaseRunning, pid};
    if (auto published = session_.region->publish(slot, beat); !published.has_value()) {
      return error_at("heartbeat", describe(published.error()));
    }
    if (elapsed_ns >= session_.pacing.duration_ns) {
      return Outcome{StopReason::kDurationElapsed, progress, elapsed_ns};
    }
    if (const auto gone = watch.gone(now.value(), elapsed_ns); gone.has_value()) {
      return Outcome{*gone, progress, elapsed_ns};
    }
    auto looked = channel.look(interval_ms);
    if (!looked.has_value()) {
      return looked.error();
    }
    auto answered = answer_all(channel, looked.value().messages);
    if (!answered.has_value()) {
      return answered.error();
    }
    const std::optional<StopReason> leaving = answered.value();
    if (leaving.has_value()) {
      return Outcome{*leaving, progress, elapsed_ns};
    }
    const std::optional<StopReason> lost = looked.value().lost;
    if (lost.has_value()) {
      return Outcome{*lost, progress, elapsed_ns};
    }
    ++progress;
  }
}

}  // namespace loadforge::worker
