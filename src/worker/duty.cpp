// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/duty.hpp"

#include <algorithm>
#include <cstdint>

#include "worker/control.hpp"

namespace loadforge::worker {

std::int64_t busy_ns(const SetDuty& duty) noexcept {
  constexpr std::uint64_t kPermille = 1000;
  const auto period = static_cast<std::uint64_t>(duty.period_ns);
  // period / 1000 whole thousandths, then the remainder's share: neither
  // product can pass the period, so nothing overflows.
  const std::uint64_t whole = period / kPermille * duty.busy_permille;
  const std::uint64_t rest = period % kPermille * duty.busy_permille / kPermille;
  return static_cast<std::int64_t>(whole + rest);
}

std::int64_t idle_ns(const SetDuty& duty) noexcept { return duty.period_ns - busy_ns(duty); }

void DutyClock::set(const SetDuty& duty, std::int64_t now_ns) noexcept {
  duty_ = duty;
  epoch_ns_ = now_ns;
}

DutyClock::Position DutyClock::at(std::int64_t now_ns) const noexcept {
  const std::int64_t busy = busy_ns(duty_);
  // A clock that went back is at the start of the wave, not before it.
  const std::int64_t into = std::max<std::int64_t>(now_ns - epoch_ns_, 0) % duty_.period_ns;
  Position position;
  position.busy = into < busy;
  position.remaining_ns = position.busy ? busy - into : duty_.period_ns - into;
  return position;
}

}  // namespace loadforge::worker
