// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the supervisor's half of the worker lifecycle.
//
// EVERY TEST HERE RUNS AT LEAST TWO WORKERS.
//
// That is not caution, it is the only way these assertions mean anything. With
// one worker, "the supervisor noticed a death" and "the supervisor noticed the
// only child exited" are the same observation: a pool that ignored pids
// entirely and reported whatever waitpid handed back would pass every
// single-worker test ever written. With two, the test can assert WHICH one died,
// and a pool that guesses fails (docs/PLAN.md §4.6).
#include "platform/worker_pool.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <string>
#include <vector>

#include "platform/exit_status.hpp"
#include "platform/process.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using loadforge::testing::FakeSyscalls;
using ForkStep = FakeSyscalls::ForkStep;
using WaitStep = FakeSyscalls::WaitStep;

constexpr pid_t kFirst = 101;
constexpr pid_t kSecond = 102;
constexpr pid_t kThird = 103;

/// Status words in the encoding the kernel actually uses -- confirmed by the
/// RealWaitStatus tier in exit_status_test.cpp, not recalled.
constexpr int exited(int code) { return (code & 0xff) << 8; }
constexpr int signalled(int sig) { return sig; }

std::vector<std::vector<std::string>> argv_for(std::size_t count) {
  std::vector<std::vector<std::string>> argv;
  for (std::size_t i = 0; i < count; ++i) {
    argv.push_back({"loadforge", "--worker", "--slot=" + std::to_string(i)});
  }
  return argv;
}

/// A pool with `count` workers already running, pids kFirst, kSecond, ...
struct RunningPool {
  explicit RunningPool(std::size_t count, std::vector<pid_t> pids = {kFirst, kSecond, kThird}) {
    std::vector<ForkStep> forks;
    for (std::size_t i = 0; i < count; ++i) {
      forks.push_back(ForkStep{pids.at(i), 0});
    }
    syscalls.script_forks(forks);
    auto outcome = pool.spawn(argv_for(count));
    spawned = outcome.has_value() && !outcome.value().is_worker();
  }

  FakeSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};
  bool spawned = false;
};

// --- spawning ----------------------------------------------------------------

TEST(WorkerPoolTest, SpawningTwoWorkersLeavesTheSupervisorHoldingBothPids) {
  RunningPool fixture{2};
  ASSERT_TRUE(fixture.spawned);

  EXPECT_EQ(fixture.pool.live_count(), 2U);
  EXPECT_EQ(fixture.pool.pid_at(0), kFirst);
  EXPECT_EQ(fixture.pool.pid_at(1), kSecond);
  EXPECT_EQ(fixture.syscalls.fork_count(), 2);
}

TEST(WorkerPoolTest, TheSupervisorOutcomeCarriesThePidsAndIsNotAWorker) {
  FakeSyscalls syscalls;
  syscalls.script_forks({ForkStep{kFirst, 0}, ForkStep{kSecond, 0}});
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(2));
  ASSERT_TRUE(outcome);
  EXPECT_FALSE(outcome.value().is_worker());
  EXPECT_EQ(outcome.value().pids(), (std::vector<pid_t>{kFirst, kSecond}));
}

TEST(WorkerPoolTest, AForkedChildLeavesTheLoopImmediatelyAndKnowsItsSlot) {
  // THE CASE THAT MATTERS MOST HERE. fork returns in both processes, so a child
  // that kept iterating would fork grandchildren and double the tree every
  // turn. The second fork returns 0, meaning "this process is now worker 1".
  FakeSyscalls syscalls;
  syscalls.script_forks({ForkStep{kFirst, 0}, ForkStep{0, 0}, ForkStep{kThird, 0}});
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(3));
  ASSERT_TRUE(outcome);
  ASSERT_TRUE(outcome.value().is_worker());
  EXPECT_EQ(outcome.value().slot(), 1U);
  EXPECT_EQ(syscalls.fork_count(), 2) << "the child must not reach the third fork";
  EXPECT_TRUE(outcome.value().pids().empty()) << "a worker created no children";
}

TEST(WorkerPoolTest, SpawningNothingSucceedsWithNoWorkers) {
  // The zero boundary. Not a useful configuration, but a loop that runs zero
  // times is a distinct path and a pool that mishandled it would fail late.
  FakeSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn({});
  ASSERT_TRUE(outcome);
  EXPECT_FALSE(outcome.value().is_worker());
  EXPECT_EQ(pool.live_count(), 0U);
  EXPECT_EQ(syscalls.fork_count(), 0);
}

// --- partial spawn failure: the pool unwinds itself --------------------------

TEST(WorkerPoolTest, AFailedForkKillsAndReapsTheWorkersAlreadyStarted) {
  // THE PATH A SUPERVISOR MUST NOT GET WRONG. If the third of three forks fails
  // and the first two are left running, they are orphans the moment this
  // process reports the error and gives up -- exactly what PR_SET_PDEATHSIG
  // exists to prevent, reached through the front door instead.
  FakeSyscalls syscalls;
  syscalls.script_forks({ForkStep{kFirst, 0}, ForkStep{kSecond, 0}, ForkStep{0, EAGAIN}});
  syscalls.script_waits({WaitStep{Reaped{kFirst, signalled(SIGKILL)}, 0},
                         WaitStep{Reaped{kSecond, signalled(SIGKILL)}, 0}});
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(3));

  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().number, EAGAIN) << "the reported error is the CAUSE, not the cleanup";
  EXPECT_EQ(outcome.error().call, "fork");

  // Both survivors were signalled...
  const auto& sent = syscalls.signals_sent();
  ASSERT_EQ(sent.size(), 2U);
  EXPECT_EQ(sent[0].first, kFirst);
  EXPECT_EQ(sent[0].second, SIGKILL);
  EXPECT_EQ(sent[1].first, kSecond);
  // ...and both were reaped, so nothing is left running or left as a zombie.
  EXPECT_EQ(syscalls.wait_count(), 2);
  EXPECT_EQ(pool.live_count(), 0U);
}

TEST(WorkerPoolTest, AForkThatFailsFirstLeavesNothingToCleanUp) {
  // The boundary of the unwind: nothing started, so nothing is signalled. A
  // cleanup loop that signalled pid 0 would be signalling the process group.
  FakeSyscalls syscalls;
  syscalls.script_forks({ForkStep{0, ENOMEM}});
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(2));

  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().number, ENOMEM);
  EXPECT_TRUE(syscalls.signals_sent().empty()) << "pid 0 means the whole process group to kill(2)";
  EXPECT_EQ(pool.live_count(), 0U);
}

TEST(WorkerPoolTest, TheUnwindStopsRatherThanSpinningWhenReapingFails) {
  // If the kernel will not tell us what happened to the survivors, the unwind
  // must give up rather than loop forever. A supervisor that hangs during
  // cleanup is worse than one that reports an incomplete cleanup.
  FakeSyscalls syscalls;
  syscalls.script_forks({ForkStep{kFirst, 0}, ForkStep{kSecond, 0}, ForkStep{0, EAGAIN}});
  syscalls.always_fail_wait(ECHILD);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(3));
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().number, EAGAIN) << "still the cause, not the cleanup's own failure";
}

// --- reaping -----------------------------------------------------------------

TEST(WorkerPoolTest, TheRightWorkerIsIdentifiedWhenOneOfTwoDies) {
  // With two workers this assertion has content: a pool that reported whatever
  // waitpid returned, without matching it to a slot, would still pass with one.
  RunningPool fixture{2};
  fixture.syscalls.script_waits({WaitStep{Reaped{kSecond, exited(3)}, 0}});

  auto death = fixture.pool.wait_for_next();
  ASSERT_TRUE(death);
  EXPECT_EQ(death.value().slot, 1U) << "the SECOND worker died, not the first";
  EXPECT_EQ(death.value().pid, kSecond);
  EXPECT_EQ(death.value().status.code(), 3);
  EXPECT_FALSE(death.value().status.is_success());

  EXPECT_EQ(fixture.pool.live_count(), 1U);
  EXPECT_EQ(fixture.pool.pid_at(0), kFirst) << "the survivor is untouched";
  EXPECT_EQ(fixture.pool.pid_at(1), 0) << "the dead slot is cleared";
}

TEST(WorkerPoolTest, DeathsAreReportedInTheOrderTheKernelDeliversThem) {
  RunningPool fixture{3};
  fixture.syscalls.script_waits({WaitStep{Reaped{kThird, signalled(SIGBUS)}, 0},
                                 WaitStep{Reaped{kFirst, exited(0)}, 0},
                                 WaitStep{Reaped{kSecond, signalled(SIGKILL)}, 0}});

  auto first = fixture.pool.wait_for_next();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().slot, 2U);
  EXPECT_EQ(first.value().status.signal(), SIGBUS);

  auto second = fixture.pool.wait_for_next();
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value().slot, 0U);
  EXPECT_TRUE(second.value().status.is_success());

  auto third = fixture.pool.wait_for_next();
  ASSERT_TRUE(third);
  EXPECT_EQ(third.value().slot, 1U);
  EXPECT_TRUE(third.value().status.killed_by_sigkill());

  EXPECT_EQ(fixture.pool.live_count(), 0U);
}

TEST(WorkerPoolTest, AChildThatIsNotOursIsReapedButNotReportedAsAWorker) {
  // A library, a shell-out, anything can leave this process a child it did not
  // create. Reporting that stranger's death as a worker's would be evidence
  // this tool invented -- and inventing evidence is the one failure a hardware
  // verifier cannot afford.
  RunningPool fixture{2};
  constexpr pid_t kStranger = 999;
  fixture.syscalls.script_waits(
      {WaitStep{Reaped{kStranger, exited(0)}, 0}, WaitStep{Reaped{kFirst, exited(7)}, 0}});

  auto death = fixture.pool.wait_for_next();
  ASSERT_TRUE(death);
  EXPECT_EQ(death.value().pid, kFirst) << "the stranger must be skipped, not reported";
  EXPECT_EQ(death.value().slot, 0U);
  EXPECT_EQ(death.value().status.code(), 7);
  EXPECT_EQ(fixture.syscalls.wait_count(), 2) << "it kept waiting rather than returning a stranger";
  EXPECT_EQ(fixture.pool.live_count(), 1U) << "the stranger must not be counted against the pool";
}

TEST(WorkerPoolTest, ReapingRetriesOnEintrAndKeepsTheDeathThatFollows) {
  RunningPool fixture{2};
  fixture.syscalls.script_waits(
      {WaitStep{{}, EINTR}, WaitStep{{}, EINTR}, WaitStep{Reaped{kSecond, exited(1)}, 0}});

  auto death = fixture.pool.wait_for_next();
  ASSERT_TRUE(death);
  EXPECT_EQ(death.value().slot, 1U);
  EXPECT_EQ(fixture.syscalls.wait_count(), 3);
}

TEST(WorkerPoolTest, AnEndlessEintrStormIsAbandonedRatherThanSpunOn) {
  // Retrying EINTR forever is the textbook answer and it is wrong here: a
  // supervisor that never returns is a HUNG tool, and the watchdog would see a
  // stalled run with no reason for it. The bound is far above any plausible
  // legitimate run of signals, so reaching it is itself evidence.
  RunningPool fixture{2};
  fixture.syscalls.always_fail_wait(EINTR);

  auto death = fixture.pool.wait_for_next();
  ASSERT_FALSE(death);
  EXPECT_EQ(death.error().number, EINTR);
  EXPECT_NE(death.error().subject.find("abandoned after"), std::string::npos);
  EXPECT_EQ(fixture.syscalls.wait_count(), WorkerPool::kMaxConsecutiveInterrupts);
}

TEST(WorkerPoolTest, ASuccessfulReapClearsTheInterruptRun) {
  // The bound is on CONSECUTIVE interruptions, not on the total. A long run on
  // a busy machine can accumulate many EINTRs over hours without any of them
  // meaning the supervisor is stuck, and a total bound would end such a run.
  RunningPool fixture{2};
  std::vector<WaitStep> steps;
  for (int i = 0; i < WorkerPool::kMaxConsecutiveInterrupts - 1; ++i) {
    steps.push_back(WaitStep{{}, EINTR});
  }
  steps.push_back(WaitStep{Reaped{kFirst, exited(0)}, 0});
  for (int i = 0; i < WorkerPool::kMaxConsecutiveInterrupts - 1; ++i) {
    steps.push_back(WaitStep{{}, EINTR});
  }
  steps.push_back(WaitStep{Reaped{kSecond, exited(0)}, 0});
  fixture.syscalls.script_waits(steps);

  EXPECT_TRUE(fixture.pool.wait_for_next()) << "just under the bound must succeed";
  EXPECT_TRUE(fixture.pool.wait_for_next())
      << "and the counter must have reset, or this second run trips the bound";
}

TEST(WorkerPoolTest, NoChildrenLeftIsReportedAsEchildRatherThanAsSuccess) {
  // The ordinary end of a run. It is an errno and not an empty result, so the
  // caller distinguishes "everything finished" from "something went wrong"
  // without having to infer it from a shape that could mean either.
  RunningPool fixture{2};
  fixture.syscalls.script_waits({});

  auto death = fixture.pool.wait_for_next();
  ASSERT_FALSE(death);
  EXPECT_EQ(death.error().number, ECHILD);
}

TEST(WorkerPoolTest, AnUnexpectedWaitErrnoIsReportedAsItself) {
  RunningPool fixture{2};
  fixture.syscalls.always_fail_wait(EINVAL);

  auto death = fixture.pool.wait_for_next();
  ASSERT_FALSE(death);
  EXPECT_EQ(death.error().number, EINVAL);
  EXPECT_EQ(fixture.syscalls.wait_count(), 1) << "only EINTR is retried";
}

// --- signalling ---------------------------------------------------------------

TEST(WorkerPoolTest, SignalAllReachesEveryLiveWorker) {
  RunningPool fixture{3};

  EXPECT_TRUE(fixture.pool.signal_all(SIGTERM));
  const auto& sent = fixture.syscalls.signals_sent();
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(sent[0], std::make_pair(kFirst, SIGTERM));
  EXPECT_EQ(sent[1], std::make_pair(kSecond, SIGTERM));
  EXPECT_EQ(sent[2], std::make_pair(kThird, SIGTERM));
}

TEST(WorkerPoolTest, AlreadyReapedSlotsAreSkipped) {
  RunningPool fixture{3};
  fixture.syscalls.script_waits({WaitStep{Reaped{kSecond, exited(0)}, 0}});
  ASSERT_TRUE(fixture.pool.wait_for_next());

  EXPECT_TRUE(fixture.pool.signal_all(SIGTERM));
  const auto& sent = fixture.syscalls.signals_sent();
  ASSERT_EQ(sent.size(), 2U) << "the reaped worker must not be signalled";
  EXPECT_EQ(sent[0].first, kFirst);
  EXPECT_EQ(sent[1].first, kThird);
}

TEST(WorkerPoolTest, EsrchOnOneWorkerIsARaceAndNotAFailure) {
  // The worker died between the last reap and this signal. No supervisor can
  // close that window, so failing on it would make signal_all fail routinely
  // during a normal shutdown.
  RunningPool fixture{3};
  fixture.syscalls.fail_signal_for(kSecond, ESRCH);

  EXPECT_TRUE(fixture.pool.signal_all(SIGTERM)) << "a vanished worker is not an error";
  EXPECT_EQ(fixture.syscalls.signals_sent().size(), 3U) << "and the rest are still signalled";
}

TEST(WorkerPoolTest, ARealSignalFailureIsReportedButTheRestAreStillSignalled) {
  // EPERM on one worker is a genuine problem worth reporting. Stopping there
  // would leave workers 2 and 3 running, which is the opposite of what a caller
  // asking to signal them ALL wants -- so the loop finishes and the first real
  // error is what comes back.
  RunningPool fixture{3};
  fixture.syscalls.fail_signal_for(kFirst, EPERM);

  auto result = fixture.pool.signal_all(SIGKILL);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().number, EPERM);
  EXPECT_EQ(result.error().call, "kill");
  EXPECT_EQ(fixture.syscalls.signals_sent().size(), 3U)
      << "every worker is signalled even after one fails";
}

TEST(WorkerPoolTest, TheFirstRealErrorIsTheOneReported) {
  // Two different failures; the first must survive. Reporting the last would
  // hand the user the consequence instead of the cause.
  RunningPool fixture{3};
  fixture.syscalls.fail_signal_for(kFirst, EPERM);
  fixture.syscalls.fail_signal_for(kThird, EINVAL);

  auto result = fixture.pool.signal_all(SIGKILL);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().number, EPERM);
}

TEST(WorkerPoolTest, SignallingAnEmptyPoolSucceedsWithoutTouchingAnything) {
  FakeSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};
  WorkerPool pool{syscalls, launcher};

  EXPECT_TRUE(pool.signal_all(SIGTERM));
  EXPECT_TRUE(syscalls.signals_sent().empty());
}

// --- accessors ----------------------------------------------------------------

TEST(WorkerPoolTest, PidAtOutOfRangeIsZeroRatherThanUndefined) {
  RunningPool fixture{2};
  EXPECT_EQ(fixture.pool.pid_at(2), 0);
  EXPECT_EQ(fixture.pool.pid_at(9999), 0);
}

// --- T8: two real processes, two real deaths ---------------------------------

TEST(RealWorkerPool, TwoRealWorkersAreSpawnedReapedAndToldApart) {
  // The whole thing against a real kernel: two children that exit with
  // DIFFERENT codes, so the test can prove the pool matched each status to the
  // right slot rather than merely counting deaths.
  RealSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/bin/false"};  // unused; the children never exec
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(2));
  ASSERT_TRUE(outcome) << describe(outcome.error());

  if (outcome.value().is_worker()) {
    // Worker 0 exits 0, worker 1 exits 5. Distinct on purpose.
    ::_exit(outcome.value().slot() == 0 ? 0 : 5);
  }

  ASSERT_EQ(pool.live_count(), 2U);
  std::vector<int> codes_by_slot(2, -1);
  for (int i = 0; i < 2; ++i) {
    auto death = pool.wait_for_next();
    ASSERT_TRUE(death) << describe(death.error());
    ASSERT_LT(death.value().slot, 2U);
    codes_by_slot[death.value().slot] = death.value().status.code();
  }

  EXPECT_EQ(codes_by_slot[0], 0);
  EXPECT_EQ(codes_by_slot[1], 5);
  EXPECT_EQ(pool.live_count(), 0U);

  auto nothing_left = pool.wait_for_next();
  ASSERT_FALSE(nothing_left);
  EXPECT_EQ(nothing_left.error().number, ECHILD);
}

TEST(RealWorkerPool, RealWorkersAreKilledBySignalAllAndClassifiedAsSignalled) {
  // Proves the signal actually lands and that the death reads back as a signal
  // rather than an exit -- the distinction the whole classifier exists for.
  RealSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/bin/false"};
  WorkerPool pool{syscalls, launcher};

  auto outcome = pool.spawn(argv_for(2));
  ASSERT_TRUE(outcome) << describe(outcome.error());

  if (outcome.value().is_worker()) {
    ::pause();   // Wait to be killed; pause only returns via a handled signal.
    ::_exit(0);  // Not reached under SIGKILL.
  }

  ASSERT_TRUE(pool.signal_all(SIGKILL));

  for (int i = 0; i < 2; ++i) {
    auto death = pool.wait_for_next();
    ASSERT_TRUE(death) << describe(death.error());
    EXPECT_EQ(death.value().status.termination(), Termination::kSignalled);
    EXPECT_TRUE(death.value().status.killed_by_sigkill()) << death.value().status.describe();
  }
  EXPECT_EQ(pool.live_count(), 0U);
}

}  // namespace
}  // namespace loadforge::platform
