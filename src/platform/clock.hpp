// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_CLOCK_HPP
#define LOADFORGE_PLATFORM_CLOCK_HPP

#include <cstdint>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// One moment, read from three clocks at once.
///
/// WHY THREE, AND NOT ONE
/// ----------------------
/// Each answers a question the others cannot, and the DIFFERENCES between them
/// are themselves evidence -- which is the only reason to pay for three reads.
///
///   raw      CLOCK_MONOTONIC_RAW. Not disciplined by NTP. The honest ruler for
///            "how long did this take", because nothing adjusts it underneath a
///            measurement in progress.
///
///   running  CLOCK_MONOTONIC. NTP-disciplined, and stops during suspend. Use
///            it when a timestamp has to line up with anything else on the
///            system, which is most reporting.
///
///   wall     CLOCK_BOOTTIME. CLOCK_MONOTONIC plus the time the machine spent
///            suspended. On its own it is rarely what you want; next to
///            `running` it is the only way to learn the machine slept.
///
/// MEASURED ON THE DEVELOPMENT MACHINE, not assumed:
///
///   CLOCK_MONOTONIC      139.916862507
///   CLOCK_MONOTONIC_RAW  139.490723645     <- 426 ms behind after 140 s
///   CLOCK_BOOTTIME       139.916895306     <- 33 us ahead of MONOTONIC
///
/// That 426 ms is NTP slew, about 0.3%, and it is the whole argument for
/// carrying `raw`: over the five-hour run this tool is built for, 0.3% is
/// nearly a minute of error in a duration a user asked to be precise.
///
/// It is also why suspend is detected from `wall - running` and NOT from
/// `wall - raw`. MONOTONIC and BOOTTIME share the same NTP discipline, so their
/// difference is suspend and nothing else; RAW does not, so `wall - raw` would
/// have reported a 426 ms "suspend" on a machine that never slept.
struct TimePoint {
  std::int64_t raw_ns = 0;
  std::int64_t running_ns = 0;
  std::int64_t wall_ns = 0;

  [[nodiscard]] friend bool operator==(const TimePoint&, const TimePoint&) = default;
};

/// What happened between two TimePoints.
struct Elapsed {
  std::int64_t raw_ns = 0;      ///< Undisciplined duration. The measurement.
  std::int64_t running_ns = 0;  ///< Time the machine was awake.

  /// Time the machine was NOT awake -- plus the skew between two clock reads,
  /// which is signed and a few microseconds wide.
  ///
  /// SO THIS CAN BE SLIGHTLY NEGATIVE, and that is not a defect. `running` and
  /// `wall` come from two separate syscalls, and the gap between them differs
  /// between the start and end readings; when it shrinks, this goes below zero.
  /// The raw figure is kept rather than clamped, because clamping would erase
  /// the only evidence of how noisy the pair of readings was.
  ///
  /// Ask machine_suspended() rather than testing this against zero.
  std::int64_t suspended_ns = 0;

  [[nodiscard]] friend bool operator==(const Elapsed&, const Elapsed&) = default;
};

/// The longest `suspended_ns` that is read skew rather than a real suspend, and
/// the shortest that is a real suspend rather than skew.
///
/// The three clocks are three separate syscalls, so their differences always
/// carry the microseconds between them. A real suspend is never sub-second -- a
/// machine that suspends and resumes inside a second has not suspended in any
/// sense that matters -- so one second separates the two cleanly.
inline constexpr std::int64_t kSuspendThresholdNs = 1'000'000'000;

/// Whether the machine slept during the interval.
///
/// Free rather than a member so Elapsed stays a plain aggregate, the same shape
/// as TimePoint and the same reasoning as core::describe(ParseError).
///
/// This matters beyond bookkeeping: a suspend invalidates every thermal
/// conclusion in the run. Silicon cools while the machine sleeps, so a
/// temperature curve spanning a suspend describes two different experiments
/// glued together, and reporting it as one would be a fabricated result.
[[nodiscard]] constexpr bool machine_suspended(const Elapsed& elapsed) noexcept {
  return elapsed.suspended_ns >= kSuspendThresholdNs;
}

/// Reads the clocks, and refuses to report a reading it cannot trust.
class Clock {
 public:
  /// Nanoseconds in a second, and the largest whole second that can be turned
  /// into nanoseconds without overflowing a signed 64-bit count.
  ///
  /// 9.2e9 seconds is about 292 years, so no real uptime approaches it. The
  /// check exists because a clock that reports nonsense must produce an error
  /// rather than a wrapped-around number that looks plausible -- the same
  /// reasoning that makes FileSystem refuse an oversized read instead of
  /// truncating it.
  static constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
  static constexpr std::int64_t kMaxSeconds = 9'223'372'035;

  explicit Clock(Syscalls& syscalls) : syscalls_(&syscalls) {}

  /// Reads all three clocks. Fails if any of them fails or reports nonsense.
  [[nodiscard]] core::Result<TimePoint, SyscallError> now();

  /// Combines a TimeSpec into nanoseconds, or says why it cannot.
  ///
  /// Free of the seam and public so its boundaries are directly testable: a
  /// nanoseconds field outside [0, 999999999] is a clock violating its own
  /// contract, and a seconds field beyond kMaxSeconds would overflow.
  [[nodiscard]] static core::Result<std::int64_t, SyscallError> to_nanoseconds(
      const TimeSpec& value, int clock_id);

  /// The interval between two readings.
  ///
  /// Fails if any clock went BACKWARDS. None of these three can do that -- that
  /// is what monotonic means -- so a negative interval is not a small
  /// measurement error to clamp away, it is evidence that the clock source is
  /// broken or that a hypervisor moved it. A stress tool that quietly reported
  /// zero there would be hiding exactly the kind of hardware misbehaviour it
  /// exists to find (F20).
  [[nodiscard]] static core::Result<Elapsed, SyscallError> between(const TimePoint& start,
                                                                   const TimePoint& end);

 private:
  Syscalls* syscalls_;
};

}  // namespace loadforge::platform

#endif
