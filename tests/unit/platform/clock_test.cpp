// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the clock, and for the thing it exists to detect.
//
// The interesting content here is not "does it read a clock". It is that the
// three clocks DISAGREE, that each disagreement means something different, and
// that a reading which cannot be trusted must be refused rather than reported.
#include "platform/clock.hpp"

#include <gtest/gtest.h>
#include <time.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kSecond = Clock::kNanosecondsPerSecond;

/// Sets all three clocks from whole seconds, for tests about the differences
/// between them rather than about sub-second arithmetic.
void set_seconds(FakeSyscalls& syscalls, std::int64_t raw, std::int64_t running,
                 std::int64_t wall) {
  syscalls.set_clock(CLOCK_MONOTONIC_RAW, TimeSpec{raw, 0});
  syscalls.set_clock(CLOCK_MONOTONIC, TimeSpec{running, 0});
  syscalls.set_clock(CLOCK_BOOTTIME, TimeSpec{wall, 0});
}

// --- to_nanoseconds: the combination, and what it refuses --------------------

TEST(ClockConversionTest, WholeSecondsAndNanosecondsCombine) {
  auto value = Clock::to_nanoseconds(TimeSpec{139, 916'862'507}, CLOCK_MONOTONIC);
  ASSERT_TRUE(value);
  EXPECT_EQ(value.value(), 139 * kSecond + 916'862'507);
}

TEST(ClockConversionTest, ZeroIsAValidReading) {
  // The boundary a machine reports in the first nanosecond after boot.
  auto value = Clock::to_nanoseconds(TimeSpec{0, 0}, CLOCK_BOOTTIME);
  ASSERT_TRUE(value);
  EXPECT_EQ(value.value(), 0);
}

TEST(ClockConversionTest, TheLargestLegalNanosecondsFieldIsAccepted) {
  auto value = Clock::to_nanoseconds(TimeSpec{1, kSecond - 1}, CLOCK_MONOTONIC);
  ASSERT_TRUE(value);
  EXPECT_EQ(value.value(), 2 * kSecond - 1);
}

TEST(ClockConversionTest, ANanosecondsFieldOfExactlyOneSecondIsRefused) {
  // clock_gettime's contract is 0 <= tv_nsec < 1e9. A value of exactly 1e9 is
  // the off-by-one a broken clock produces, and it combines into a number that
  // looks entirely plausible -- which is what makes refusing it worthwhile.
  auto value = Clock::to_nanoseconds(TimeSpec{1, kSecond}, CLOCK_MONOTONIC);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().number, EINVAL);
  EXPECT_NE(value.error().subject.find("outside [0, 1e9)"), std::string::npos);
}

TEST(ClockConversionTest, ANegativeNanosecondsFieldIsRefused) {
  auto value = Clock::to_nanoseconds(TimeSpec{1, -1}, CLOCK_MONOTONIC);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().number, EINVAL);
}

TEST(ClockConversionTest, ANegativeSecondCountIsRefused) {
  // No monotonic clock can be negative. A hypervisor that moved the clock is
  // the realistic cause, and it is exactly the hardware misbehaviour this tool
  // must report rather than absorb.
  auto value = Clock::to_nanoseconds(TimeSpec{-1, 0}, CLOCK_BOOTTIME);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().number, EINVAL);
  EXPECT_NE(value.error().subject.find("negative"), std::string::npos);
}

TEST(ClockConversionTest, TheLargestSecondCountThatFitsIsAccepted) {
  auto value = Clock::to_nanoseconds(TimeSpec{Clock::kMaxSeconds, 0}, CLOCK_MONOTONIC);
  ASSERT_TRUE(value);
  EXPECT_GT(value.value(), 0) << "it must not have wrapped";
}

TEST(ClockConversionTest, ASecondCountThatWouldOverflowIsRefused) {
  // The other side of that boundary. 9.2e9 seconds is about 292 years, so this
  // is unreachable by a real uptime -- but a wrapped negative duration reads as
  // a perfectly plausible measurement, and that is the failure worth refusing.
  auto value = Clock::to_nanoseconds(TimeSpec{Clock::kMaxSeconds + 1, 0}, CLOCK_MONOTONIC);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().number, EOVERFLOW);
  EXPECT_NE(value.error().subject.find("does not fit"), std::string::npos);
}

TEST(ClockConversionTest, TheFailureNamesTheClockThatProducedIt) {
  auto value = Clock::to_nanoseconds(TimeSpec{0, -5}, CLOCK_BOOTTIME);
  ASSERT_FALSE(value);
  EXPECT_NE(value.error().subject.find(std::to_string(CLOCK_BOOTTIME)), std::string::npos)
      << "three clocks are read; a message that does not say which is nearly useless";
}

// --- now(): all three, and every way a read can fail -------------------------

TEST(ClockTest, AllThreeClocksAreReadAndKeptSeparate) {
  FakeSyscalls syscalls;
  syscalls.set_clock(CLOCK_MONOTONIC_RAW, TimeSpec{139, 490'723'645});
  syscalls.set_clock(CLOCK_MONOTONIC, TimeSpec{139, 916'862'507});
  syscalls.set_clock(CLOCK_BOOTTIME, TimeSpec{139, 916'895'306});
  Clock clock{syscalls};

  auto point = clock.now();
  ASSERT_TRUE(point);
  EXPECT_EQ(point.value().raw_ns, 139 * kSecond + 490'723'645);
  EXPECT_EQ(point.value().running_ns, 139 * kSecond + 916'862'507);
  EXPECT_EQ(point.value().wall_ns, 139 * kSecond + 916'895'306);
}

TEST(ClockTest, RunningAndWallAreReadAdjacently) {
  // Their difference is the suspend evidence, and the gap between the two
  // syscalls is the noise in it. Reading them next to each other keeps that
  // noise as small as the kernel allows; a third read wedged between them would
  // widen it for no reason.
  FakeSyscalls syscalls;
  Clock clock{syscalls};
  ASSERT_TRUE(clock.now());

  const auto& order = syscalls.clocks_read();
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], CLOCK_MONOTONIC_RAW);
  EXPECT_EQ(order[1], CLOCK_MONOTONIC);
  EXPECT_EQ(order[2], CLOCK_BOOTTIME);
}

TEST(ClockTest, AFailureFromAnyOneClockFailsTheWholeReading) {
  // A TimePoint with two good fields and one stale zero would be worse than no
  // reading: every difference computed from it would be wrong by the whole
  // uptime, and nothing in the result would say so.
  for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
    FakeSyscalls syscalls;
    set_seconds(syscalls, 100, 100, 100);
    syscalls.fail_clock(clock_id, EINVAL);
    Clock clock{syscalls};

    auto point = clock.now();
    ASSERT_FALSE(point) << "clock " << clock_id;
    EXPECT_EQ(point.error().number, EINVAL);
    EXPECT_EQ(point.error().call, "clock_gettime");
  }
}

TEST(ClockTest, AnUnsupportedClockIsReportedRatherThanSkipped) {
  // EINVAL is what the kernel returns for a clock it does not know --
  // CLOCK_BOOTTIME needs Linux 2.6.39, and this tool is meant to run on
  // whatever a user has. Reporting it lets the caller say which clock is
  // missing; silently dropping to two clocks would lose suspend detection with
  // no announcement.
  FakeSyscalls syscalls;
  syscalls.fail_clock(CLOCK_BOOTTIME, EINVAL);
  Clock clock{syscalls};

  auto point = clock.now();
  ASSERT_FALSE(point);
  EXPECT_NE(point.error().subject.find(std::to_string(CLOCK_BOOTTIME)), std::string::npos);
}

TEST(ClockTest, ANonsenseReadingFromAClockFailsTheWholeReading) {
  FakeSyscalls syscalls;
  set_seconds(syscalls, 100, 100, 100);
  syscalls.set_clock(CLOCK_MONOTONIC, TimeSpec{100, kSecond + 1});
  Clock clock{syscalls};

  auto point = clock.now();
  ASSERT_FALSE(point);
  EXPECT_EQ(point.error().number, EINVAL);
}

// --- between(): the measurement, and the suspend ------------------------------

TEST(ClockElapsedTest, AnOrdinaryIntervalReportsNoSuspend) {
  const TimePoint start{10 * kSecond, 10 * kSecond, 10 * kSecond};
  const TimePoint end{70 * kSecond, 70 * kSecond, 70 * kSecond};

  auto elapsed = Clock::between(start, end);
  ASSERT_TRUE(elapsed);
  EXPECT_EQ(elapsed.value().raw_ns, 60 * kSecond);
  EXPECT_EQ(elapsed.value().running_ns, 60 * kSecond);
  EXPECT_EQ(elapsed.value().suspended_ns, 0);
  EXPECT_FALSE(machine_suspended(elapsed.value()));
}

TEST(ClockElapsedTest, ASuspendShowsAsWallRunningAheadOfMonotonic) {
  // THE CASE THIS MODULE EXISTS FOR. The machine ran for 60 seconds and the
  // world moved on by 3660 -- an hour asleep with the lid shut. Every thermal
  // number spanning that gap describes two experiments glued together.
  const TimePoint start{10 * kSecond, 10 * kSecond, 10 * kSecond};
  const TimePoint end{70 * kSecond, 70 * kSecond, 3670 * kSecond};

  auto elapsed = Clock::between(start, end);
  ASSERT_TRUE(elapsed);
  EXPECT_EQ(elapsed.value().running_ns, 60 * kSecond);
  EXPECT_EQ(elapsed.value().suspended_ns, 3600 * kSecond);
  EXPECT_TRUE(machine_suspended(elapsed.value()));
}

TEST(ClockElapsedTest, MicrosecondsOfReadSkewAreNotReportedAsASuspend) {
  // The three clocks are three separate syscalls, so `wall - running` always
  // carries the microseconds between them. Without a threshold every single
  // run would claim the machine had suspended -- a false positive on the one
  // signal that invalidates a run's thermal conclusions.
  const TimePoint start{0, 0, 0};
  const TimePoint end{60 * kSecond, 60 * kSecond, 60 * kSecond + 33'000};

  auto elapsed = Clock::between(start, end);
  ASSERT_TRUE(elapsed);
  EXPECT_EQ(elapsed.value().suspended_ns, 33'000) << "the skew is still REPORTED";
  EXPECT_FALSE(machine_suspended(elapsed.value())) << "it is just not called a suspend";
}

TEST(ClockElapsedTest, TheSuspendThresholdIsABoundaryAndIsTestedOnBothSides) {
  const TimePoint start{0, 0, 0};

  const TimePoint just_under{0, 0, kSuspendThresholdNs - 1};
  auto under = Clock::between(start, just_under);
  ASSERT_TRUE(under);
  EXPECT_FALSE(machine_suspended(under.value()));

  const TimePoint exactly{0, 0, kSuspendThresholdNs};
  auto at = Clock::between(start, exactly);
  ASSERT_TRUE(at);
  EXPECT_TRUE(machine_suspended(at.value()));
}

TEST(ClockElapsedTest, RawIsReportedSeparatelyBecauseItDisagreesWithMonotonic) {
  // Measured on the development machine: after 140 seconds of uptime,
  // CLOCK_MONOTONIC_RAW was 426ms behind CLOCK_MONOTONIC -- about 0.3% NTP
  // slew. Over the five-hour run this tool is built for that is nearly a
  // minute, which is why the undisciplined ruler is carried rather than
  // assumed equal to the disciplined one.
  const TimePoint start{0, 0, 0};
  const TimePoint end{139'490'723'645, 139'916'862'507, 139'916'895'306};

  auto elapsed = Clock::between(start, end);
  ASSERT_TRUE(elapsed);
  EXPECT_EQ(elapsed.value().raw_ns, 139'490'723'645);
  EXPECT_EQ(elapsed.value().running_ns, 139'916'862'507);
  EXPECT_NE(elapsed.value().raw_ns, elapsed.value().running_ns)
      << "if these ever agree exactly, one of the two clocks is not what we think";
  EXPECT_FALSE(machine_suspended(elapsed.value()))
      << "and the raw/monotonic gap must NOT be mistaken for a suspend";
}

TEST(ClockElapsedTest, AZeroLengthIntervalIsValid) {
  const TimePoint point{5 * kSecond, 5 * kSecond, 5 * kSecond};
  auto elapsed = Clock::between(point, point);
  ASSERT_TRUE(elapsed);
  EXPECT_EQ(elapsed.value().raw_ns, 0);
  EXPECT_EQ(elapsed.value().running_ns, 0);
  EXPECT_FALSE(machine_suspended(elapsed.value()));
}

TEST(ClockElapsedTest, EachClockGoingBackwardsIsRefusedAndNamed) {
  // A monotonic clock cannot go backwards; that is what the word means. So a
  // negative interval is not a rounding error to clamp to zero, it is evidence
  // that the clock source is broken or a hypervisor moved it -- exactly the
  // hardware misbehaviour this tool exists to find. Clamping would hide it.
  const TimePoint start{100 * kSecond, 100 * kSecond, 100 * kSecond};

  const struct {
    TimePoint end;
    const char* named;
  } cases[] = {
      {{99 * kSecond, 100 * kSecond, 100 * kSecond}, "CLOCK_MONOTONIC_RAW"},
      {{100 * kSecond, 99 * kSecond, 100 * kSecond}, "CLOCK_MONOTONIC"},
      {{100 * kSecond, 100 * kSecond, 99 * kSecond}, "CLOCK_BOOTTIME"},
  };

  for (const auto& testcase : cases) {
    auto elapsed = Clock::between(start, testcase.end);
    ASSERT_FALSE(elapsed) << testcase.named;
    EXPECT_EQ(elapsed.error().number, EINVAL);
    EXPECT_NE(elapsed.error().subject.find(testcase.named), std::string::npos)
        << "got: " << elapsed.error().subject;
    EXPECT_NE(elapsed.error().subject.find("went backwards"), std::string::npos);
  }
}

TEST(ClockElapsedTest, ASmallNegativeSuspendIsReadSkewAndIsAccepted) {
  // THE CASE THAT CORRECTED THIS MODULE. An earlier version refused any
  // negative value, reasoning that BOOTTIME is MONOTONIC plus suspend and so
  // can never advance less. True of the two CLOCKS at one instant; false of two
  // READINGS taken at different instants, which is what between() actually has.
  //
  // `running` and `wall` are separate syscalls, so each TimePoint carries a gap
  // between them -- and the gap differs between the start and end readings.
  // When it shrinks, this difference goes negative by microseconds. The real
  // clocks produced -2989ns under the sanitizer presets, which are slow enough
  // to vary the skew more, and the strict check failed there while passing in
  // the debug build.
  const TimePoint start{0, 0, 0};
  const TimePoint end{60 * kSecond, 60 * kSecond, 60 * kSecond - 2'989};

  auto elapsed = Clock::between(start, end);
  ASSERT_TRUE(elapsed) << "a few microseconds of read skew is not a broken clock";
  EXPECT_EQ(elapsed.value().suspended_ns, -2'989) << "and the skew is reported, not clamped away";
  EXPECT_FALSE(machine_suspended(elapsed.value()));
}

TEST(ClockElapsedTest, ANegativeSuspendBeyondAWholeSecondIsAContradictionAndIsRefused) {
  // The other side of that tolerance, and it is symmetric with the suspend
  // threshold for the same reason: skew is bounded by the microseconds between
  // two syscalls, so a negative second cannot be skew. It means these are not
  // the clocks this code believes they are.
  const TimePoint start{0, 0, 0};
  const TimePoint end{60 * kSecond, 60 * kSecond, 58 * kSecond};  // wall 2s short

  auto elapsed = Clock::between(start, end);
  ASSERT_FALSE(elapsed);
  EXPECT_EQ(elapsed.error().number, EINVAL);
  EXPECT_NE(elapsed.error().subject.find("beyond the skew"), std::string::npos);
}

TEST(ClockElapsedTest, TheNegativeToleranceIsABoundaryAndIsTestedOnBothSides) {
  const TimePoint start{0, 0, 0};

  // Both clocks must still move FORWARD -- a backwards clock is a different
  // failure, checked above -- so the contradiction is built from a wall delta
  // that is positive but smaller than the running delta.
  const TimePoint at_limit{0, 2 * kSecond, kSecond};
  auto at = Clock::between(start, at_limit);
  ASSERT_TRUE(at) << "exactly the tolerance is still accepted";
  EXPECT_EQ(at.value().suspended_ns, -kSuspendThresholdNs);

  const TimePoint past_limit{0, 2 * kSecond, kSecond - 1};
  EXPECT_FALSE(Clock::between(start, past_limit)) << "one nanosecond past it is refused";
}

// --- T8: the real clocks ------------------------------------------------------

TEST(RealClock, ReadsAllThreeRealClocksAndTheyAreOrderedAsDocumented) {
  RealSyscalls syscalls;
  Clock clock{syscalls};

  auto point = clock.now();
  ASSERT_TRUE(point) << describe(point.error());
  EXPECT_GT(point.value().raw_ns, 0);
  EXPECT_GT(point.value().running_ns, 0);
  EXPECT_GT(point.value().wall_ns, 0);

  // BOOTTIME >= MONOTONIC always, because it is MONOTONIC plus suspend. This is
  // asserted against the kernel rather than assumed, because the whole suspend
  // calculation rests on it.
  EXPECT_GE(point.value().wall_ns, point.value().running_ns);
}

TEST(RealClock, TimeActuallyPassesBetweenTwoRealReadings) {
  RealSyscalls syscalls;
  Clock clock{syscalls};

  auto start = clock.now();
  ASSERT_TRUE(start) << describe(start.error());
  // Enough work to be certain the clock moved, without a sleep that would make
  // the suite slower for no gain.
  volatile std::int64_t sink = 0;
  for (int i = 0; i < 1'000'000; ++i) {
    sink += i;
  }
  auto end = clock.now();
  ASSERT_TRUE(end) << describe(end.error());

  auto elapsed = Clock::between(start.value(), end.value());
  ASSERT_TRUE(elapsed) << describe(elapsed.error());
  EXPECT_GT(elapsed.value().raw_ns, 0) << "the raw clock did not move at all";
  EXPECT_GT(elapsed.value().running_ns, 0);
  EXPECT_FALSE(machine_suspended(elapsed.value()))
      << "a suspend inside a test is not credible; suspended_ns was "
      << elapsed.value().suspended_ns;
}

TEST(RealClock, AnUnknownClockIdIsEINVALFromTheRealKernel) {
  // The errno the capability model depends on, taken from the kernel rather
  // than from a fake that would have agreed with whatever was assumed.
  RealSyscalls syscalls;
  auto value = syscalls.read_clock(9999);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().number, EINVAL);
  EXPECT_EQ(value.error().call, "clock_gettime");
}

}  // namespace
}  // namespace loadforge::platform
