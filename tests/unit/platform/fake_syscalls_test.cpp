// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared fake's own tests. A fake that is laxer, or stricter, than the
// kernel hides bugs above the seam (docs/JOURNAL.md §1.23, §1.27), so the one
// place where it models the life of a child -- running, zombie, reaped -- is
// pinned here, and the kernel side of each row is pinned in
// real_syscalls_test.cpp (KillSucceedsOnAnUnreapedZombieAndOnlyTheReapMakesItESRCH).

#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <vector>

#include "platform/syscalls.hpp"

namespace loadforge::testing {
namespace {

using platform::Reaped;

constexpr pid_t kChild = 777;

TEST(FakeSyscallsTest, ARunningChildTakesASignalAndHasNotExited) {
  FakeSyscalls fake;
  EXPECT_TRUE(fake.send_signal(kChild, SIGTERM).has_value());
  const auto peek = fake.has_exited(kChild);
  ASSERT_TRUE(peek.has_value());
  EXPECT_FALSE(peek.value());
  EXPECT_EQ(fake.exit_peeks(), std::vector<pid_t>{kChild});
}

TEST(FakeSyscallsTest, AZombieTakesASignalWithSuccessAsTheKernelDoes) {
  FakeSyscalls fake;
  fake.child_exits(kChild);
  EXPECT_TRUE(fake.is_zombie(kChild));
  const auto sent = fake.send_signal(kChild, SIGKILL);
  EXPECT_TRUE(sent.has_value()) << "kill(2) succeeds on a zombie, not ESRCH";
  const auto peek = fake.has_exited(kChild);
  ASSERT_TRUE(peek.has_value());
  EXPECT_TRUE(peek.value());
  EXPECT_FALSE(fake.is_reaped(kChild)) << "asking does not reap";
}

TEST(FakeSyscallsTest, OnlyTheReapTurnsASignalIntoESRCHAndAPeekIntoECHILD) {
  FakeSyscalls fake;
  fake.child_exits(kChild);
  fake.script_nonblocking_reaps({{Reaped{kChild, 0}, 0}});
  const auto reaped = fake.reap_any_nonblocking();
  ASSERT_TRUE(reaped.has_value());
  ASSERT_TRUE(reaped.value().has_value());
  EXPECT_TRUE(fake.is_reaped(kChild));
  EXPECT_FALSE(fake.is_zombie(kChild));
  const auto sent = fake.send_signal(kChild, SIGKILL);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().number, ESRCH);
  const auto peek = fake.has_exited(kChild);
  ASSERT_FALSE(peek.has_value());
  EXPECT_EQ(peek.error().number, ECHILD);
  EXPECT_EQ(peek.error().call, "waitid");
}

TEST(FakeSyscallsTest, ABlockingReapAlsoEndsTheChild) {
  FakeSyscalls fake;
  fake.script_waits({{Reaped{kChild, 0}, 0}});
  ASSERT_TRUE(fake.wait_any().has_value());
  EXPECT_TRUE(fake.is_reaped(kChild));
  EXPECT_EQ(fake.send_signal(kChild, SIGTERM).error().number, ESRCH);
}

TEST(FakeSyscallsTest, AReapThatFailsEndsNothing) {
  FakeSyscalls fake;
  fake.child_exits(kChild);
  fake.script_nonblocking_reaps({{Reaped{}, EINTR}});
  ASSERT_FALSE(fake.reap_any_nonblocking().has_value());
  EXPECT_FALSE(fake.is_reaped(kChild));
  EXPECT_TRUE(fake.is_zombie(kChild));
}

TEST(FakeSyscallsTest, AProcessGroupIsNotAChildAndKeepsItsOwnRule) {
  FakeSyscalls fake;
  fake.script_nonblocking_reaps({{Reaped{kChild, 0}, 0}});
  ASSERT_TRUE(fake.reap_any_nonblocking().has_value());
  // The group of a reaped worker answers by the group rule (empty: ESRCH).
  EXPECT_EQ(fake.send_signal(-kChild, SIGKILL).error().number, ESRCH);
  fake.group_left_running(kChild);
  EXPECT_TRUE(fake.send_signal(-kChild, SIGKILL).has_value());
}

TEST(FakeSyscallsTest, AScriptedPeekFailureIsRaisedAndRecorded) {
  FakeSyscalls fake;
  fake.fail_has_exited(EINVAL);
  const auto peek = fake.has_exited(kChild);
  ASSERT_FALSE(peek.has_value());
  EXPECT_EQ(peek.error().number, EINVAL);
}

TEST(FakeSyscallsTest, ASlowForkMovesEveryClockAndATestCanSeeWhatTheNextReadWillSay) {
  FakeSyscalls fake;
  fake.advance_clocks_per_read(10'000'000);
  fake.script_forks({{kChild, 0}, {kChild + 1, 0}});
  fake.delay_each_fork(995'000'000);
  std::vector<int> returned;
  fake.on_fork_returned([&returned](int index) { returned.push_back(index); });
  EXPECT_EQ(fake.peek_clock_ns(CLOCK_MONOTONIC), 0) << "peeking reads nothing";
  EXPECT_EQ(fake.peek_clock_ns(CLOCK_MONOTONIC), 0);
  ASSERT_TRUE(fake.fork_process().has_value());
  EXPECT_EQ(returned, std::vector<int>{0});
  EXPECT_EQ(fake.peek_clock_ns(CLOCK_MONOTONIC), 995'000'000);
  // The nanoseconds carry into the seconds, as the real clock's do.
  ASSERT_TRUE(fake.read_clock(CLOCK_MONOTONIC).has_value());
  const auto second = fake.read_clock(CLOCK_MONOTONIC);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value().seconds, 1);
  EXPECT_EQ(second.value().nanoseconds, 5'000'000);
  fake.script_forks({{0, 0}});
  ASSERT_TRUE(fake.fork_process().has_value());
  EXPECT_EQ(returned, std::vector<int>{0}) << "a child returns from no parent's fork";
}

}  // namespace
}  // namespace loadforge::testing
