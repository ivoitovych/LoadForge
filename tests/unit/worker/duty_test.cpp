// SPDX-License-Identifier: GPL-3.0-or-later
//
// The square wave of thermal and power cycling: how much of a period is busy,
// and where in the wave an instant falls.

#include "worker/duty.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace loadforge::worker {
namespace {

constexpr std::int64_t kMs = 1'000'000;

TEST(DutyArithmetic, ABusyShareIsThatManyThousandthsOfThePeriodRoundedDown) {
  EXPECT_EQ(busy_ns(SetDuty{1000, 1000 * kMs}), 1000 * kMs) << "continuous load";
  EXPECT_EQ(busy_ns(SetDuty{500, 1000 * kMs}), 500 * kMs);
  EXPECT_EQ(busy_ns(SetDuty{1, 1000 * kMs}), 1 * kMs);
  EXPECT_EQ(busy_ns(SetDuty{250, 7}), 1) << "7 * 250 / 1000 = 1.75, down";
  EXPECT_EQ(busy_ns(SetDuty{1, 999}), 0) << "less than a nanosecond is none";
  EXPECT_EQ(busy_ns(SetDuty{1000, 1}), 1);
  EXPECT_EQ(idle_ns(SetDuty{250, 1000 * kMs}), 750 * kMs);
  EXPECT_EQ(idle_ns(SetDuty{1000, 1000 * kMs}), 0) << "continuous load never idles";
}

TEST(DutyArithmetic, ACenturyLongPeriodDoesNotOverflow) {
  constexpr std::int64_t kMost = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(busy_ns(SetDuty{1000, kMost}), kMost) << "all of it";
  const std::int64_t half = busy_ns(SetDuty{500, kMost});
  EXPECT_NEAR(static_cast<double>(half), static_cast<double>(kMost) / 2.0, 1000.0);
  EXPECT_EQ(busy_ns(SetDuty{1, kMost}), kMost / 1000);
  EXPECT_EQ(idle_ns(SetDuty{500, kMost}), kMost - half);
}

TEST(DutyClockTest, WithoutADutyTheLoadIsContinuous) {
  const DutyClock clock;
  for (const std::int64_t now : {std::int64_t{0}, 1 * kMs, 999 * kMs, 1000 * kMs, 5000 * kMs}) {
    EXPECT_TRUE(clock.at(now).busy) << now;
  }
}

TEST(DutyClockTest, ABusyPartThenAnIdlePartInEveryPeriodFromTheInstantItWasSet) {
  DutyClock clock;
  clock.set(SetDuty{300, 1000 * kMs}, 5000 * kMs);
  struct Case {
    std::int64_t at;
    bool busy;
    std::int64_t remaining;
  };
  const Case cases[] = {
      {5000 * kMs, true, 300 * kMs},                  // The wave begins with its busy part.
      {5100 * kMs, true, 200 * kMs},                  //
      {5299 * kMs, true, 1 * kMs},                    //
      {5300 * kMs, false, 700 * kMs},                 // The first nanosecond past the busy part.
      {5999 * kMs, false, 1 * kMs},                   //
      {6000 * kMs, true, 300 * kMs},                  // The second period.
      {6450 * kMs, false, 550 * kMs},                 //
      {5000 * kMs + 3'000'000'000, true, 300 * kMs},  // Three periods on.
  };
  for (const Case& each : cases) {
    const DutyClock::Position position = clock.at(each.at);
    EXPECT_EQ(position.busy, each.busy) << each.at;
    EXPECT_EQ(position.remaining_ns, each.remaining) << each.at;
  }
}

TEST(DutyClockTest, ACameraBackwardsClockIsAtTheStartOfTheWaveNotBeforeIt) {
  DutyClock clock;
  clock.set(SetDuty{500, 1000 * kMs}, 5000 * kMs);
  const DutyClock::Position position = clock.at(4000 * kMs);
  EXPECT_TRUE(position.busy);
  EXPECT_EQ(position.remaining_ns, 500 * kMs);
}

TEST(DutyClockTest, ASecondDutyReplacesTheFirstFromItsOwnInstant) {
  DutyClock clock;
  clock.set(SetDuty{100, 1000 * kMs}, 0);
  clock.set(SetDuty{1000, 1000 * kMs}, 250 * kMs);
  EXPECT_TRUE(clock.at(900 * kMs).busy) << "continuous again";
  clock.set(SetDuty{1, 1000 * kMs}, 900 * kMs);
  EXPECT_TRUE(clock.at(900 * kMs).busy);
  EXPECT_FALSE(clock.at(902 * kMs).busy);
}

TEST(DutyClockTest, ABusyPartOfNoTimeIsNeverBusy) {
  DutyClock clock;
  clock.set(SetDuty{1, 999}, 0);  // A thousandth of 999 ns is none.
  EXPECT_FALSE(clock.at(0).busy);
  EXPECT_EQ(clock.at(0).remaining_ns, 999);
}

}  // namespace
}  // namespace loadforge::worker
