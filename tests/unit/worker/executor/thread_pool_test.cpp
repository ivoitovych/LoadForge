// SPDX-License-Identifier: GPL-3.0-or-later
//
// The thread pool: the executor contract against it (T1), every refusal the
// factory reports forced through the two seams (T2), and the boundaries of
// what it is given (P3). The ordering argument and its tests are in
// ordering_test.cpp; the real kernel's answers in tests/integration.

#include "worker/executor/thread_pool.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "platform/affinity.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/worker/executor/fakes.hpp"
#include "worker/executor.hpp"

namespace loadforge::worker {
namespace {

using platform::CpuMask;
using testing::ArmFpControl;
using testing::ScriptedThreads;
using testing::X86FpControl;

CpuMask cpus(const std::vector<std::uint32_t>& ids) { return platform::mask_of(ids).value(); }

std::vector<ThreadPlacement> unpinned(std::size_t threads) {
  return std::vector<ThreadPlacement>(threads);
}

// ---- the contract, against the pool ---------------------------------------
//
// Unpinned, so the suite runs on any machine and any cpuset; the integration
// tier runs it again with every thread pinned to a CPU of its own. The seams
// are static because the suite's make() is, and a pool needs its Threads for
// as long as it lives.

template <std::size_t kThreads>
struct UnpinnedPool {
  static std::unique_ptr<Executor> make() {
    static PosixThreads threads;
    static loadforge::testing::FakeSyscalls syscalls;
    static const X86FpControl fp;
    return make_thread_pool(unpinned(kThreads), threads, syscalls, fp).value();
  }
};

// ---- placements ---------------------------------------------------------------

TEST(OneThreadPerCpu, OnePlacementPerCpuEachNamingThatCpuAloneInOrder) {
  const std::vector<ThreadPlacement> placements = one_thread_per_cpu(cpus({5, 1, 1023}));
  ASSERT_EQ(placements.size(), 3U);
  EXPECT_EQ(placements[0], cpus({1}));
  EXPECT_EQ(placements[1], cpus({5}));
  EXPECT_EQ(placements[2], cpus({1023}));
}

TEST(OneThreadPerCpu, AnEmptyMaskGivesNoPlacements) {
  EXPECT_TRUE(one_thread_per_cpu(CpuMask{}).empty());
}

// ---- the factory: a pool, and its boundaries --------------------------------

TEST(MakeThreadPool, NoPlacementsIsRefusedAndStartsNothing) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const auto made = make_thread_pool({}, threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  EXPECT_TRUE(std::holds_alternative<NoThreads>(made.error()));
  EXPECT_EQ(threads.attempts(), 0U);
  EXPECT_EQ(fp.reads(), 0U) << "nothing is read for a pool that cannot exist";
  EXPECT_EQ(describe(made.error()), "an executor needs at least one thread, and none was placed");
}

TEST(MakeThreadPool, OnePlacementIsOneThread) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  {
    const auto made = make_thread_pool(unpinned(1), threads, syscalls, fp);
    ASSERT_TRUE(made.has_value());
    EXPECT_EQ(made.value()->threads(), 1U);
    EXPECT_EQ(threads.started(), 1U);
  }
  EXPECT_EQ(threads.joined(), 1U);
}

TEST(MakeThreadPool, EachPlacedThreadPinsItselfAndReadsItsOwnRegisters) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const std::vector<ThreadPlacement> placements = {cpus({2}), std::nullopt, cpus({0, 3})};
  {
    const auto made = make_thread_pool(placements, threads, syscalls, fp);
    ASSERT_TRUE(made.has_value()) << describe(made.error());
    EXPECT_EQ(made.value()->threads(), 3U);
    // Two placements, two calls, each for the calling thread -- pid 0 -- and
    // never for a pid: affinity is set by the thread on itself. The fake has
    // one register for "the calling thread", and the last thread placed wrote
    // it; the unpinned thread between them asked for nothing.
    EXPECT_EQ(syscalls.affinity_pids(), (std::vector<pid_t>{0, 0}));
    EXPECT_EQ(syscalls.current_affinity(), cpus({0, 3}));
    // The creator's read, then one per thread.
    EXPECT_EQ(fp.reads(), 4U);
  }
  EXPECT_EQ(threads.started(), 3U);
  EXPECT_EQ(threads.joined(), 3U);
}

TEST(MakeThreadPool, AnUnusedPoolStopsAndJoinsEveryThread) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const ArmFpControl fp;
  {
    const auto made = make_thread_pool(unpinned(5), threads, syscalls, fp);
    ASSERT_TRUE(made.has_value());
  }
  EXPECT_EQ(threads.started(), 5U);
  EXPECT_EQ(threads.joined(), 5U);
}

// ---- P2: the system will not start a thread ----------------------------------
//
// pthread_create(3) documents three: EAGAIN (a limit on threads, or no memory
// for one), EINVAL and EPERM (an attribute refused; the pool passes none, and
// a refusal it did not expect is still reported, never swallowed).

class ThreadNotStartedTest : public ::testing::TestWithParam<int> {};

TEST_P(ThreadNotStartedTest, TheFirstThreadRefusedStartsNothingAndJoinsNothing) {
  ScriptedThreads threads(0, GetParam());
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const auto made = make_thread_pool(unpinned(3), threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  const auto* refused = std::get_if<ThreadNotStarted>(&made.error());
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->thread, 0U);
  EXPECT_EQ(refused->cause, (platform::SyscallError{GetParam(), "pthread_create", "thread 0"}));
  EXPECT_EQ(threads.attempts(), 1U) << "no start is tried after a refusal";
  EXPECT_EQ(threads.joined(), 0U);
  EXPECT_EQ(describe(made.error()),
            "cannot start executor thread 0: " + platform::describe(refused->cause));
}

TEST_P(ThreadNotStartedTest, ALaterThreadRefusedJoinsEveryThreadAlreadyStarted) {
  ScriptedThreads threads(2, GetParam());
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const auto made = make_thread_pool(unpinned(4), threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  const auto* refused = std::get_if<ThreadNotStarted>(&made.error());
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->thread, 2U);
  EXPECT_EQ(refused->cause, (platform::SyscallError{GetParam(), "pthread_create", "thread 2"}));
  EXPECT_EQ(threads.attempts(), 3U);
  EXPECT_EQ(threads.started(), 2U);
  EXPECT_EQ(threads.joined(), 2U) << "the two threads that did start are stopped and reaped";
}

INSTANTIATE_TEST_SUITE_P(EachDocumentedError, ThreadNotStartedTest,
                         ::testing::Values(EAGAIN, EINVAL, EPERM));

// ---- P2: the kernel will not place a thread ----------------------------------
//
// sched_setaffinity(2) documents EFAULT, EINVAL, EPERM and ESRCH, and
// platform::Affinity names each; the pool carries that classification, with
// which thread it was, rather than making one of its own.

struct PlacementCase {
  int error;
  platform::Placement kind;
};

class ThreadNotPlacedTest : public ::testing::TestWithParam<PlacementCase> {};

TEST_P(ThreadNotPlacedTest, TheRefusedThreadIsJoinedAndTheRefusalCarried) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  syscalls.fail_set_affinity(GetParam().error);
  const X86FpControl fp;
  const auto made = make_thread_pool({cpus({1}), cpus({2})}, threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  const auto* refused = std::get_if<ThreadNotPlaced>(&made.error());
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->thread, 0U);
  EXPECT_EQ(refused->refused.kind, GetParam().kind);
  EXPECT_EQ(refused->refused.requested, cpus({1}));
  EXPECT_EQ(refused->refused.cause,
            (platform::SyscallError{GetParam().error, "sched_setaffinity", "pid 0"}));
  EXPECT_EQ(threads.attempts(), 1U) << "the second thread is never started";
  EXPECT_EQ(threads.joined(), 1U) << "the refused thread has returned and is reaped";
  EXPECT_EQ(fp.reads(), 1U) << "a thread that was not placed does not go on to check";
  EXPECT_EQ(describe(made.error()), "executor thread 0: " + platform::describe(refused->refused));
}

INSTANTIATE_TEST_SUITE_P(EachDocumentedError, ThreadNotPlacedTest,
                         ::testing::Values(PlacementCase{EINVAL, platform::Placement::kNoUsableCpu},
                                           PlacementCase{ESRCH, platform::Placement::kThreadGone},
                                           PlacementCase{EPERM, platform::Placement::kNotPermitted},
                                           PlacementCase{EFAULT,
                                                         platform::Placement::kUnexpected}));

// The P7 state affinity exists for, met by a pool: the worker was placed on
// CPUs the topology found and the cpuset withholds one of them. The kernel's
// EINVAL arrives at the thread placed there, after two were placed and
// started -- and both are stopped and reaped.
TEST(MakeThreadPool, ACpuTheCpusetWithholdsRefusesThatThreadAndUnwindsTheRest) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  syscalls.permit_cpus(cpus({0, 1}));
  const X86FpControl fp;
  const auto made = make_thread_pool(one_thread_per_cpu(cpus({0, 1, 2})), threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  const auto* refused = std::get_if<ThreadNotPlaced>(&made.error());
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->thread, 2U);
  EXPECT_EQ(refused->refused.kind, platform::Placement::kNoUsableCpu);
  EXPECT_EQ(refused->refused.requested, cpus({2}));
  EXPECT_EQ(threads.started(), 3U);
  EXPECT_EQ(threads.joined(), 3U);
}

// ---- the floating-point state a thread starts with ---------------------------

TEST(MakeThreadPool, AThreadWhoseStateIsNotItsCreatorsIsRefusedByName) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  X86FpControl fp;
  fpenv::FpState changed = fpenv::required_state(true, false);
  changed.flush_to_zero = true;
  fp.dirty_from(2, changed);  // read 0 is the creator's, read 1 thread 0's
  const auto made = make_thread_pool(unpinned(3), threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  const auto* differs = std::get_if<ThreadFpStateDiffers>(&made.error());
  ASSERT_NE(differs, nullptr);
  EXPECT_EQ(differs->thread, 1U);
  EXPECT_EQ(differs->expected, fpenv::required_state(true, false));
  EXPECT_EQ(differs->found, changed);
  EXPECT_EQ(threads.attempts(), 2U);
  EXPECT_EQ(threads.joined(), 2U);
  EXPECT_EQ(describe(made.error()),
            "executor thread 1 started with a floating-point state other than its creator's: "
            "expected rounding to nearest, ties to even, flush-to-zero off, denormals-are-zero "
            "off, default-NaN absent; found rounding to nearest, ties to even, flush-to-zero on, "
            "denormals-are-zero off, default-NaN absent");
}

// The expected state is whatever the creator holds -- not the required one,
// which is the worker's to establish and check before it makes the pool. A
// creator in an unusual state passes it on, and its threads agree with it.
TEST(MakeThreadPool, TheExpectedStateIsTheCreatorsWhateverItIs) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  ArmFpControl fp;
  fpenv::FpState unusual = fpenv::required_state(false, true);
  unusual.rounding = fpenv::Rounding::kUpward;
  unusual.default_nan = true;
  fp.write(unusual);
  const auto made = make_thread_pool(unpinned(2), threads, syscalls, fp);
  ASSERT_TRUE(made.has_value());
}

TEST(MakeThreadPool, EveryPropertyOfADifferentStateIsNamed) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  ArmFpControl fp;
  const fpenv::FpState changed{fpenv::Rounding::kDownward, true, std::nullopt, true};
  fp.dirty_from(1, changed);
  const auto made = make_thread_pool(unpinned(1), threads, syscalls, fp);
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(describe(made.error()),
            "executor thread 0 started with a floating-point state other than its creator's: "
            "expected rounding to nearest, ties to even, flush-to-zero off, denormals-are-zero "
            "absent, default-NaN off; found rounding downward, flush-to-zero on, "
            "denormals-are-zero absent, default-NaN on");
  EXPECT_EQ(threads.joined(), 1U);
}

// ---- runs ---------------------------------------------------------------------

TEST(ThreadPoolRun, ThreadsIsThePlacementCountForTheLifeOfThePool) {
  ScriptedThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const auto made = make_thread_pool(unpinned(6), threads, syscalls, fp);
  ASSERT_TRUE(made.has_value());
  Executor& executor = *made.value();
  EXPECT_EQ(executor.threads(), 6U);
  auto nothing = [](std::size_t, std::size_t) noexcept {};
  for (const std::size_t count : {0U, 1U, 5U, 6U, 7U, 100U}) {
    for_each_index(executor, count, nothing);
    EXPECT_EQ(executor.threads(), 6U) << count;
  }
}

}  // namespace
}  // namespace loadforge::worker

// In the suite's own namespace: the instantiation names the suite unqualified.
namespace loadforge::contracts {
namespace {

// One thread (every run's last thread out is its only one), a power of two,
// and seven: a count no partition divides evenly (docs/determinism.md §5).
using Pools =
    ::testing::Types<worker::UnpinnedPool<1>, worker::UnpinnedPool<4>, worker::UnpinnedPool<7>>;
INSTANTIATE_TYPED_TEST_SUITE_P(ThreadPool, ExecutorContract, Pools);

}  // namespace
}  // namespace loadforge::contracts
