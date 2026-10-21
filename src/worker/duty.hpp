// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_DUTY_HPP
#define LOADFORGE_WORKER_DUTY_HPP

// The square wave of thermal and power cycling (worker/control.hpp, SetDuty):
// from the instant a duty is set, in every period of period_ns the worker runs
// slices for the first busy_permille thousandths of it and idles the rest.
// A pure function of the duty and of two readings of a clock, so the controller
// and the worker argue from the same arithmetic about how long a worker may
// legitimately make no progress.

#include <cstdint>

#include "worker/control.hpp"

namespace loadforge::worker {

/// How much of a period is busy, in nanoseconds: busy_permille thousandths of
/// it, rounded down, computed so that a period of centuries cannot overflow.
[[nodiscard]] std::int64_t busy_ns(const SetDuty& duty) noexcept;

/// How much of a period is idle: the rest of it.
[[nodiscard]] std::int64_t idle_ns(const SetDuty& duty) noexcept;

class DutyClock {
 public:
  /// Where in the wave an instant falls: whether the worker runs, and how long
  /// until that changes.
  struct Position {
    bool busy = true;
    std::int64_t remaining_ns = 0;
  };

  /// Continuous load until a duty is set.
  DutyClock() noexcept = default;

  /// From `now_ns` on, the wave begins with its busy part.
  void set(const SetDuty& duty, std::int64_t now_ns) noexcept;

  [[nodiscard]] Position at(std::int64_t now_ns) const noexcept;

 private:
  SetDuty duty_;
  std::int64_t epoch_ns_ = 0;
};

}  // namespace loadforge::worker

#endif
