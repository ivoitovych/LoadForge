// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the worker launcher and executable-path resolution.
//
// Most of this drives a fake, because that is the only way to reach the paths
// that matter: a fork(2) that fails, a prctl that fails, a supervisor that dies
// inside the window between the fork and the arming of the death signal. None
// of those can be provoked on demand against a real kernel, and every one of
// them is a path a worker takes on a machine that is already misbehaving --
// which is the only kind of machine this tool is ever pointed at.
//
// The RealWorker tier at the bottom then forks genuine children and execs
// genuine binaries, because a fake agrees with whatever the code believes (F21).
#include "platform/process.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <string>
#include <vector>

#include "platform/exit_status.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using loadforge::testing::FakeSyscalls;

constexpr pid_t kSupervisor = FakeSyscalls::kFakeSupervisorPid;
constexpr pid_t kChildPid = 4242;

// --- ForkOutcome: the type that exists to stop `if (pid == 0)` ---------------

TEST(ForkOutcomeTest, ChildIsChildAndReportsNoPid) {
  const ForkOutcome outcome = ForkOutcome::child();
  EXPECT_TRUE(outcome.is_child());
  // 0 is not a pid, so a caller that ignores is_child() and uses this gets a
  // value that cannot be mistaken for a real process.
  EXPECT_EQ(outcome.child_pid(), 0);
}

TEST(ForkOutcomeTest, ParentIsNotChildAndCarriesThePid) {
  const ForkOutcome outcome = ForkOutcome::parent(kChildPid);
  EXPECT_FALSE(outcome.is_child());
  EXPECT_EQ(outcome.child_pid(), kChildPid);
}

// --- fork_worker -------------------------------------------------------------

TEST(WorkerLauncherTest, ForkReturningZeroIsTheChildSide) {
  FakeSyscalls syscalls;
  syscalls.fork_yields_child();
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  auto outcome = launcher.fork_worker();
  ASSERT_TRUE(outcome);
  EXPECT_TRUE(outcome.value().is_child());
  EXPECT_EQ(syscalls.fork_count(), 1);
}

TEST(WorkerLauncherTest, ForkReturningAPidIsTheParentSide) {
  FakeSyscalls syscalls;
  syscalls.fork_yields_parent(kChildPid);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  auto outcome = launcher.fork_worker();
  ASSERT_TRUE(outcome);
  EXPECT_FALSE(outcome.value().is_child());
  EXPECT_EQ(outcome.value().child_pid(), kChildPid);
}

TEST(WorkerLauncherTest, ForkFailureIsReportedWithItsErrno) {
  // EAGAIN is the realistic one: the process or user hit RLIMIT_NPROC, which a
  // tool that spawns one worker per core can genuinely do on a large machine.
  FakeSyscalls syscalls;
  syscalls.fail_fork(EAGAIN);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  auto outcome = launcher.fork_worker();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().number, EAGAIN);
  EXPECT_EQ(outcome.error().call, "fork");
}

TEST(WorkerLauncherTest, EveryDocumentedForkErrnoIsCarriedThrough) {
  // fork(2) documents these. None is a branch a coverage tool can see, and each
  // one reaches a user as a different explanation of why no worker started.
  //
  // ERESTARTNOINTR is deliberately absent although fork(2) lists it: it is
  // kernel-internal and never reaches userspace -- the kernel restarts the call
  // instead of returning it, and glibc does not even define the constant. The
  // compiler is what established that, by refusing to name it.
  for (const int number : {EAGAIN, ENOMEM, ENOSYS, EPERM}) {
    FakeSyscalls syscalls;
    syscalls.fail_fork(number);
    WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

    auto outcome = launcher.fork_worker();
    ASSERT_FALSE(outcome) << "errno " << number;
    EXPECT_EQ(outcome.error().number, number);
  }
}

// --- become_worker: the child's half ----------------------------------------

TEST(WorkerLauncherTest, TheChildArmsTheDeathSignalWithSigkill) {
  FakeSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  const WorkerStartupFailure failure = launcher.become_worker(kSupervisor, {"loadforge", "--work"});

  EXPECT_EQ(syscalls.pdeathsig_count(), 1);
  EXPECT_EQ(syscalls.last_death_signal(), SIGKILL)
      << "SIGTERM would let a worker ignore the one guarantee against orphans";
  EXPECT_EQ(WorkerLauncher::kParentDeathSignal, SIGKILL);
  // Reaching exec means the arming and the race check both passed.
  EXPECT_EQ(failure, WorkerStartupFailure::kExecFailed);
}

TEST(WorkerLauncherTest, TheChildExecsTheGivenBinaryWithTheGivenArguments) {
  FakeSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  const std::vector<std::string> argv{"loadforge", "--worker", "--slot=3"};
  (void)launcher.become_worker(kSupervisor, argv);

  EXPECT_EQ(syscalls.exec_count(), 1);
  EXPECT_EQ(syscalls.last_exec_path(), "/usr/bin/loadforge")
      << "the path must come from the launcher, never from argv[0]";
  EXPECT_EQ(syscalls.last_exec_argv(), argv);
}

TEST(WorkerLauncherTest, AFailedExecIsReportedWithACodeAndKeepsTheErrno) {
  FakeSyscalls syscalls;
  syscalls.fail_exec(ENOENT);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}), WorkerStartupFailure::kExecFailed);
  // The code is all that crosses the process boundary; the detail is kept for
  // the child to report before it exits.
  EXPECT_EQ(launcher.last_exec_error().number, ENOENT);
  EXPECT_EQ(launcher.last_exec_error().call, "execv");
  EXPECT_EQ(launcher.last_exec_error().subject, "/usr/bin/loadforge");
}

TEST(WorkerLauncherTest, EveryDocumentedExecErrnoIsKept) {
  for (const int number : {ENOENT, EACCES, ENOEXEC, EIO, ELOOP, ENAMETOOLONG, ENOMEM, ETXTBSY,
                           EISDIR, ENOTDIR, EPERM, E2BIG}) {
    FakeSyscalls syscalls;
    syscalls.fail_exec(number);
    WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

    EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}),
              WorkerStartupFailure::kExecFailed);
    EXPECT_EQ(launcher.last_exec_error().number, number) << "errno " << number;
  }
}

TEST(WorkerLauncherTest, AFailedDeathSignalStopsTheWorkerBeforeItExecs) {
  // If the kernel will not arm PR_SET_PDEATHSIG the orphan guarantee is gone,
  // and a worker that cannot be killed with its supervisor is worse than no
  // worker: it pins a core on a machine whose owner has lost the tool driving
  // it. So this refuses to become a worker rather than continuing without it.
  FakeSyscalls syscalls;
  syscalls.fail_set_parent_death_signal(EINVAL);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}),
            WorkerStartupFailure::kDeathSignalUnavailable);
  EXPECT_EQ(syscalls.exec_count(), 0) << "it must not exec after refusing to start";
}

TEST(WorkerLauncherTest, ASupervisorThatDiedInTheRaceWindowIsDetected) {
  // THE RACE. PR_SET_PDEATHSIG fires when the parent dies -- but if the parent
  // died between the fork and the arming, the death it was armed for has
  // already happened and no signal will ever come. An orphan is reparented, so
  // its parent pid stops matching the supervisor that forked it.
  FakeSyscalls syscalls;
  syscalls.set_parent_pid(1);  // reparented to init
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}),
            WorkerStartupFailure::kParentDiedBeforeArming);
  EXPECT_EQ(syscalls.exec_count(), 0) << "an orphan must not go on to do work";
  EXPECT_EQ(syscalls.pdeathsig_count(), 1)
      << "the signal is armed first, to make the unguarded window as short as possible";
}

TEST(WorkerLauncherTest, TheRaceCheckDoesNotAssumeInitIsTheReaper) {
  // Comparing against pid 1 would be wrong under a subreaper, in a container, or
  // under anything that called PR_SET_CHILD_SUBREAPER: an orphan is reparented
  // to the nearest subreaper, which is not init. The check therefore compares
  // against the pid captured before the fork, whatever the new parent is.
  FakeSyscalls syscalls;
  syscalls.set_parent_pid(7777);  // a subreaper, not init
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}),
            WorkerStartupFailure::kParentDiedBeforeArming);
}

TEST(WorkerLauncherTest, AMatchingParentPidProceedsToExec) {
  // The other side of the boundary above: same supervisor, so no reparenting
  // happened and the worker goes on.
  FakeSyscalls syscalls;
  syscalls.set_parent_pid(kSupervisor);
  WorkerLauncher launcher{syscalls, "/usr/bin/loadforge"};

  EXPECT_EQ(launcher.become_worker(kSupervisor, {"loadforge"}), WorkerStartupFailure::kExecFailed);
  EXPECT_EQ(syscalls.exec_count(), 1);
}

TEST(WorkerLauncherTest, StartupFailureCodesAreDistinctAndSurviveExit) {
  // These cross a process boundary as a single byte. exit(2) keeps the low 8
  // bits only, so a code above 255 would report as something else entirely --
  // and ExitStatus proves exit(256) reads back as success.
  const int exec_failed = static_cast<int>(WorkerStartupFailure::kExecFailed);
  const int orphaned = static_cast<int>(WorkerStartupFailure::kParentDiedBeforeArming);
  const int no_signal = static_cast<int>(WorkerStartupFailure::kDeathSignalUnavailable);

  EXPECT_NE(exec_failed, orphaned);
  EXPECT_NE(exec_failed, no_signal);
  EXPECT_NE(orphaned, no_signal);

  for (const int code : {exec_failed, orphaned, no_signal}) {
    EXPECT_GT(code, 2) << "must not collide with success or ordinary failure";
    EXPECT_LT(code, 126) << "126 and 127 are the shell's, and >255 wraps to something else";
    EXPECT_EQ(code & 0xff, code) << "must survive exit(2)'s truncation to one byte";
  }
}

// --- ExecutablePath ----------------------------------------------------------

TEST(ExecutablePathTest, AResolvedLinkIsReturned) {
  FakeSyscalls syscalls;
  syscalls.set_link_target("/usr/local/bin/loadforge");
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved.value(), "/usr/local/bin/loadforge");
  EXPECT_EQ(syscalls.read_link_count(), 1);
}

TEST(ExecutablePathTest, ItReadsProcSelfExeByDefault) {
  FakeSyscalls syscalls;
  ExecutablePath path{syscalls};
  (void)path.resolve();
  EXPECT_EQ(syscalls.last_path(), "/proc/self/exe");
}

TEST(ExecutablePathTest, ATruncatedLinkIsRefusedRatherThanReturnedShort) {
  // THE CASE THIS CLASS EXISTS FOR. readlink does not null-terminate and does
  // not report truncation: it fills the buffer and returns its size, which is
  // indistinguishable from an exact fit. A truncated path is not a broken
  // string -- it is a valid path to the WRONG FILE, and execing the wrong file
  // is how a supervisor ends up supervising something it did not build.
  FakeSyscalls syscalls;
  syscalls.set_link_target(std::string(ExecutablePath::kMaxPathLength + 1, 'a'));
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_FALSE(resolved);
  EXPECT_EQ(resolved.error().number, ENAMETOOLONG);
}

TEST(ExecutablePathTest, ALinkExactlyFillingTheBufferIsAlsoRefused) {
  // The boundary, and a deliberate choice: a target of exactly kMaxPathLength
  // bytes is indistinguishable from a truncated one, because readlink reports
  // the same count either way. Refusing costs the one path of exactly that
  // length and buys the guarantee that nothing silently shortened is returned.
  FakeSyscalls syscalls;
  syscalls.set_link_target(std::string(ExecutablePath::kMaxPathLength, 'a'));
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_FALSE(resolved);
  EXPECT_EQ(resolved.error().number, ENAMETOOLONG);
}

TEST(ExecutablePathTest, ALinkOneByteUnderTheLimitIsAccepted) {
  // The other side of that boundary, so the refusal above is a limit and not an
  // off-by-one that rejects everything long.
  FakeSyscalls syscalls;
  const std::string target(ExecutablePath::kMaxPathLength - 1, 'a');
  syscalls.set_link_target(target);
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved.value().size(), ExecutablePath::kMaxPathLength - 1);
  EXPECT_EQ(resolved.value(), target);
}

TEST(ExecutablePathTest, AnEmptyLinkIsRefusedRatherThanReturnedAsAPath) {
  // Linux does not produce this for /proc/self/exe, but an empty string would
  // be accepted downstream as a path and fail much later with no clue where it
  // came from. F20: refuse what cannot be interpreted.
  FakeSyscalls syscalls;
  syscalls.set_link_target("");
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_FALSE(resolved);
  EXPECT_EQ(resolved.error().number, ENOENT);
  EXPECT_NE(resolved.error().subject.find("empty path"), std::string::npos);
}

TEST(ExecutablePathTest, EveryDocumentedReadlinkErrnoIsCarriedThrough) {
  // ENOENT is the one that matters most: it is what a live medium without /proc
  // mounted produces, and the design forbids falling back to argv[0] there.
  for (const int number : {ENOENT, EACCES, ENOTDIR, EINVAL, ELOOP, ENAMETOOLONG, EIO, ENOMEM}) {
    FakeSyscalls syscalls;
    syscalls.fail_read_link(number);
    ExecutablePath path{syscalls};

    auto resolved = path.resolve();
    ASSERT_FALSE(resolved) << "errno " << number;
    EXPECT_EQ(resolved.error().number, number);
    EXPECT_EQ(resolved.error().call, "readlink");
  }
}

TEST(ExecutablePathTest, AMissingProcIsReportedAndNotSilentlyWorkedAround) {
  // Stated as its own test because it is a design decision, not an error path:
  // there is no argv[0] fallback. A live medium without /proc is exactly where
  // execing the wrong binary would be hardest to notice.
  FakeSyscalls syscalls;
  syscalls.fail_read_link(ENOENT);
  ExecutablePath path{syscalls};

  auto resolved = path.resolve("/proc/self/exe");
  ASSERT_FALSE(resolved);
  EXPECT_EQ(resolved.error().subject, "/proc/self/exe");
}

// --- T8: against the real kernel --------------------------------------------

TEST(RealWorker, ResolvesThisTestBinarysOwnPath) {
  RealSyscalls syscalls;
  ExecutablePath path{syscalls};

  auto resolved = path.resolve();
  ASSERT_TRUE(resolved) << describe(resolved.error());
  EXPECT_EQ(resolved.value().front(), '/') << "must be absolute";
  EXPECT_NE(resolved.value().find("loadforge_unit_tests"), std::string::npos)
      << "resolved to " << resolved.value();
}

TEST(RealWorker, ForksAndExecsARealBinaryThatExitsCleanly) {
  RealSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/bin/true"};
  const pid_t supervisor = ::getpid();

  auto outcome = launcher.fork_worker();
  ASSERT_TRUE(outcome) << describe(outcome.error());

  if (outcome.value().is_child()) {
    // The two lines of glue that cannot be driven from a fake, because _exit
    // would take the test process with it. This is why the decisions live above
    // the seam and only this remains.
    ::_exit(static_cast<int>(launcher.become_worker(supervisor, {"true"})));
  }

  int raw = 0;
  ASSERT_EQ(::waitpid(outcome.value().child_pid(), &raw, 0), outcome.value().child_pid());
  const ExitStatus status{raw};
  EXPECT_TRUE(status.is_success()) << status.describe();
}

TEST(RealWorker, AFailedExecReachesTheSupervisorAsItsDistinguishedCode) {
  // End to end, through a real process boundary: the child cannot exec, returns
  // kExecFailed, exits with it, and the supervisor reads that exact code back.
  // This is what proves the code survives exit(2) in practice and not only in
  // the arithmetic asserted above.
  RealSyscalls syscalls;
  WorkerLauncher launcher{syscalls, "/nonexistent/loadforge-worker"};
  const pid_t supervisor = ::getpid();

  auto outcome = launcher.fork_worker();
  ASSERT_TRUE(outcome) << describe(outcome.error());

  if (outcome.value().is_child()) {
    ::_exit(static_cast<int>(launcher.become_worker(supervisor, {"loadforge"})));
  }

  int raw = 0;
  ASSERT_EQ(::waitpid(outcome.value().child_pid(), &raw, 0), outcome.value().child_pid());
  const ExitStatus status{raw};
  EXPECT_EQ(status.termination(), Termination::kExited);
  EXPECT_EQ(status.code(), static_cast<int>(WorkerStartupFailure::kExecFailed));
  EXPECT_FALSE(status.is_success());
}

TEST(RealWorker, TheDeathSignalIsActuallyArmedInARealChild) {
  // prctl(PR_SET_PDEATHSIG) succeeding is asserted against a real kernel rather
  // than only against the fake, because the fake cannot tell us whether the
  // kernel accepts the call at all -- on a seccomp profile that blocks prctl it
  // would not, and the worker would refuse to start for a reason no unit test
  // could have predicted.
  RealSyscalls syscalls;
  auto armed = syscalls.set_parent_death_signal(SIGKILL);
  EXPECT_TRUE(armed) << (armed ? "" : describe(armed.error()));

  // Disarm, so this test process does not carry a death signal into the rest of
  // the suite. 0 means "no signal", which is how PR_SET_PDEATHSIG is cleared.
  auto cleared = syscalls.set_parent_death_signal(0);
  EXPECT_TRUE(cleared);
}

}  // namespace
}  // namespace loadforge::platform
