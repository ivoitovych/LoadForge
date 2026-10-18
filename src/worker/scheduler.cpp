// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/clock.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"

namespace loadforge::worker {

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
  }
  return "unrecognised stop reason";
}

bool controller_lost(StopReason reason) noexcept {
  return reason == StopReason::kControllerHungUp || reason == StopReason::kControllerStale ||
         reason == StopReason::kControllerUnreadable;
}

std::string describe(const WorkerError& error) { return error.stage + ": " + error.detail; }

namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;

WorkerError error_at(std::string_view stage, std::string detail) {
  return WorkerError{std::string{stage}, std::move(detail)};
}

}  // namespace

std::optional<StopReason> NullWorkload::controller_gone(const platform::TimePoint& now,
                                                        std::int64_t elapsed_ns) const {
  // A reader that cannot get a consistent sample has a controller that died
  // inside publish(); a stale one has a controller that stopped beating; and
  // one that never beat within the allowance is treated as stale, because a
  // controller publishes its first beat before it spawns anyone.
  auto observed = session_.region->observe(ipc::kControllerSlot);
  if (!observed.has_value()) {
    return StopReason::kControllerUnreadable;
  }
  const ipc::Liveness liveness =
      ipc::liveness(observed.value(), now.running_ns, session_.pacing.stale_after_ns);
  if (liveness == ipc::Liveness::kStale ||
      (liveness == ipc::Liveness::kNeverBeat && elapsed_ns > session_.pacing.stale_after_ns)) {
    return StopReason::kControllerStale;
  }
  return std::nullopt;
}

core::Result<std::optional<StopReason>, WorkerError> NullWorkload::wait_on_control(
    int interval_ms) {
  const std::vector<int> watched{session_.control_fd};
  auto ready = syscalls_->poll_readable(watched, interval_ms);
  if (!ready.has_value()) {
    return error_at("control pipe", describe(ready.error()));
  }
  const platform::Readiness& control = ready.value().front();
  if (control.hung_up || control.error || control.invalid) {
    return std::optional<StopReason>{StopReason::kControllerHungUp};
  }
  if (!control.readable) {
    return std::optional<StopReason>{};  // The interval passed.
  }
  // A byte is a stop request; end of file is the writer gone, which poll
  // reports as readable on some kernels before it reports the hang-up.
  std::array<char, 1> byte{};
  auto count = syscalls_->read(session_.control_fd, byte.data(), byte.size());
  if (!count.has_value()) {
    return error_at("control pipe", describe(count.error()));
  }
  if (count.value() == 0) {
    return std::optional<StopReason>{StopReason::kControllerHungUp};
  }
  return std::optional<StopReason>{StopReason::kStopRequested};
}

core::Result<Outcome, WorkerError> NullWorkload::run() {
  platform::Clock clock(*syscalls_);
  auto started = clock.now();
  if (!started.has_value()) {
    return error_at("clock", describe(started.error()));
  }
  const auto pid = static_cast<std::uint32_t>(syscalls_->process_id());
  const std::size_t slot = ipc::worker_slot(session_.slot);
  const auto interval_ms =
      static_cast<int>(session_.pacing.interval_ns / kNanosecondsPerMillisecond);
  std::uint64_t progress = 0;

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
    if (const auto gone = controller_gone(now.value(), elapsed_ns); gone.has_value()) {
      return Outcome{*gone, progress, elapsed_ns};
    }
    auto waited = wait_on_control(interval_ms);
    if (!waited.has_value()) {
      return waited.error();
    }
    const std::optional<StopReason> stopped = waited.value();
    if (stopped.has_value()) {
      return Outcome{*stopped, progress, elapsed_ns};
    }
    ++progress;
  }
}

}  // namespace loadforge::worker
