// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/clock.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <string>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {
namespace {

/// A monotonic reading that went backwards, reported as EINVAL against the
/// clock that did it. There is no errno for "your clock is broken", and
/// inventing one would be worse than reusing the one that means "this value is
/// not valid".
core::Result<std::int64_t, SyscallError> forward_difference(std::int64_t start, std::int64_t end,
                                                            const char* which) {
  if (end < start) {
    return SyscallError{
        EINVAL, "clock_gettime",
        std::string{which} + " went backwards by " + std::to_string(start - end) + "ns"};
  }
  return end - start;
}

}  // namespace

core::Result<std::int64_t, SyscallError> Clock::to_nanoseconds(const TimeSpec& value,
                                                               int clock_id) {
  // clock_gettime's contract is 0 <= tv_nsec < 1e9. A value outside that is a
  // clock violating its own specification, and the combined figure would be
  // silently wrong rather than obviously wrong -- so it is refused.
  if (value.nanoseconds < 0 || value.nanoseconds >= kNanosecondsPerSecond) {
    return SyscallError{EINVAL, "clock_gettime",
                        "clock " + std::to_string(clock_id) + " reported " +
                            std::to_string(value.nanoseconds) + "ns, outside [0, 1e9)"};
  }
  if (value.seconds < 0) {
    return SyscallError{EINVAL, "clock_gettime",
                        "clock " + std::to_string(clock_id) + " reported a negative second count"};
  }
  if (value.seconds > kMaxSeconds) {
    return SyscallError{EOVERFLOW, "clock_gettime",
                        "clock " + std::to_string(clock_id) + " reported " +
                            std::to_string(value.seconds) + "s, which does not fit in nanoseconds"};
  }
  return value.seconds * kNanosecondsPerSecond + value.nanoseconds;
}

core::Result<TimePoint, SyscallError> Clock::now() {
  // Read in this order and keep it: `running` and `wall` are the pair whose
  // difference is the suspend evidence, so they are read next to each other to
  // make the gap between them -- which shows up as noise in that difference --
  // as small as possible.
  struct Reading {
    int id;
    std::int64_t* field;
  };

  TimePoint point;
  const std::array<Reading, 3> readings{{
      {CLOCK_MONOTONIC_RAW, &point.raw_ns},
      {CLOCK_MONOTONIC, &point.running_ns},
      {CLOCK_BOOTTIME, &point.wall_ns},
  }};

  for (const Reading& reading : readings) {
    auto value = syscalls_->read_clock(reading.id);
    if (!value) {
      return value.error();
    }
    auto nanoseconds = to_nanoseconds(value.value(), reading.id);
    if (!nanoseconds) {
      return nanoseconds.error();
    }
    *reading.field = nanoseconds.value();
  }
  return point;
}

core::Result<Elapsed, SyscallError> Clock::between(const TimePoint& start, const TimePoint& end) {
  auto raw = forward_difference(start.raw_ns, end.raw_ns, "CLOCK_MONOTONIC_RAW");
  if (!raw) {
    return raw.error();
  }
  auto running = forward_difference(start.running_ns, end.running_ns, "CLOCK_MONOTONIC");
  if (!running) {
    return running.error();
  }
  auto wall = forward_difference(start.wall_ns, end.wall_ns, "CLOCK_BOOTTIME");
  if (!wall) {
    return wall.error();
  }

  // suspended = (wall - running) is suspend time PLUS READ SKEW, and the skew is
  // SIGNED.
  //
  // The two clocks are read by two separate syscalls, so each TimePoint carries
  // a small gap between them -- and that gap differs between the two readings.
  // When it shrinks, the BOOTTIME delta comes out microseconds *smaller* than
  // the MONOTONIC one and this difference goes negative.
  //
  // An earlier version refused any negative value, reasoning that BOOTTIME is
  // MONOTONIC plus suspend and so can never advance less. That is true of the
  // two CLOCKS at one instant and false of two READINGS taken at different
  // instants, which is what this function actually has. It failed under the
  // sanitizer presets -- slower, so more skew variance -- reporting the clocks
  // as contradictory by 2989ns.
  //
  // So the tolerance is symmetric with the suspend threshold: skew is bounded by
  // the microseconds between two syscalls, a real suspend is never sub-second,
  // and a NEGATIVE value beyond a whole second cannot be either. That is a
  // genuine contradiction and is still refused.
  const std::int64_t suspended = wall.value() - running.value();
  if (suspended < -kSuspendThresholdNs) {
    return SyscallError{EINVAL, "clock_gettime",
                        "CLOCK_BOOTTIME advanced less than CLOCK_MONOTONIC by " +
                            std::to_string(-suspended) +
                            "ns, far beyond the skew between two reads"};
  }

  return Elapsed{raw.value(), running.value(), suspended};
}

}  // namespace loadforge::platform
