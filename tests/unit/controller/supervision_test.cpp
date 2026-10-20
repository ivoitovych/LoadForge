// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classification table: every row of the misbehaviour catalogue is one
// Evidence and one fate, and the fates that must never be confused are not.

#include "controller/supervision.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <optional>

#include "platform/exit_status.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::ExitStatus;

// The status words waitpid(2) writes, as the ExitStatus tests read them off
// real children: exit code in the high byte; a signal in the low seven bits.
constexpr ExitStatus exited(int code) { return ExitStatus{code << 8}; }
constexpr ExitStatus signalled(int signal) { return ExitStatus{signal}; }

Evidence after(ExitStatus status) {
  Evidence evidence;
  evidence.status = status;
  return evidence;
}

TEST(FateWordsTest, EveryFateHasWordsAndOnlyCompletedIsClean) {
  EXPECT_EQ(describe(WorkerFate::kCompleted), "completed");
  EXPECT_EQ(describe(WorkerFate::kWorkerError), "worker error");
  EXPECT_EQ(describe(WorkerFate::kStartupFailed), "startup failed");
  EXPECT_EQ(describe(WorkerFate::kControllerLostReported), "worker reported the controller lost");
  EXPECT_EQ(describe(WorkerFate::kFault), "fault");
  EXPECT_EQ(describe(WorkerFate::kKilled), "killed");
  EXPECT_EQ(describe(WorkerFate::kStoppedByController), "stopped by the controller");
  EXPECT_EQ(describe(WorkerFate::kTimedOut), "timed out");
  EXPECT_EQ(describe(WorkerFate::kChannelCorrupt), "record channel corrupt");
  EXPECT_EQ(describe(WorkerFate::kTruncatedEvidence), "evidence truncated");
  EXPECT_EQ(describe(WorkerFate::kUnrecognised), "unrecognised termination");
  EXPECT_EQ(describe(static_cast<WorkerFate>(200)), "unrecognised fate");

  EXPECT_TRUE(is_clean(WorkerFate::kCompleted));
  for (const WorkerFate fate :
       {WorkerFate::kWorkerError, WorkerFate::kStartupFailed, WorkerFate::kControllerLostReported,
        WorkerFate::kFault, WorkerFate::kKilled, WorkerFate::kStoppedByController,
        WorkerFate::kTimedOut, WorkerFate::kChannelCorrupt, WorkerFate::kTruncatedEvidence,
        WorkerFate::kUnrecognised}) {
    EXPECT_FALSE(is_clean(fate)) << describe(fate);
  }
}

TEST(FateWordsTest, TheFaultSignalsAreTheSixTheKernelDeliversForAFault) {
  for (const int signal : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP}) {
    EXPECT_TRUE(is_fault_signal(signal)) << signal;
  }
  for (const int signal : {SIGKILL, SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGUSR1}) {
    EXPECT_FALSE(is_fault_signal(signal)) << signal;
  }
}

TEST(FateWordsTest, EvidenceComparesFieldByField) {
  const Evidence a = after(exited(0));
  Evidence b = a;
  EXPECT_EQ(a, b);
  b.status = exited(1);
  EXPECT_NE(a, b);
  b = a;
  b.signal_sent = SIGTERM;
  EXPECT_NE(a, b);
  b = a;
  b.stale_when_signalled = true;
  EXPECT_NE(a, b);
  b = a;
  b.finished_seen = true;
  EXPECT_NE(a, b);
  b = a;
  b.channel_corrupt = true;
  EXPECT_NE(a, b);
  b = a;
  b.channel_truncated = true;
  EXPECT_NE(a, b);
}

TEST(ClassifyTest, ACleanExitIsCompleteOnlyWithItsFinishedRecordInHand) {
  Evidence finished = after(exited(0));
  finished.finished_seen = true;
  EXPECT_EQ(classify(finished), WorkerFate::kCompleted);

  EXPECT_EQ(classify(after(exited(0))), WorkerFate::kTruncatedEvidence)
      << "exit 0 and no finished record: no evidence that it finished";

  Evidence cut = finished;
  cut.channel_truncated = true;
  EXPECT_EQ(classify(cut), WorkerFate::kTruncatedEvidence)
      << "a stream that ended inside a frame, even after a finished record";
}

TEST(ClassifyTest, ExitCodesAreTheWorkersOwnTheLaunchersOrTheControllersFault) {
  EXPECT_EQ(classify(after(exited(1))), WorkerFate::kWorkerError);
  EXPECT_EQ(classify(after(exited(3))), WorkerFate::kWorkerError);
  EXPECT_EQ(classify(after(exited(119))), WorkerFate::kWorkerError)
      << "the worker's own setup failure";
  for (const int code : {120, 121, 122, 123, 125}) {
    EXPECT_EQ(classify(after(exited(code))), WorkerFate::kStartupFailed) << code;
  }
  EXPECT_EQ(classify(after(exited(124))), WorkerFate::kControllerLostReported);
  EXPECT_EQ(classify(after(exited(126))), WorkerFate::kWorkerError);
  EXPECT_EQ(classify(after(exited(255))), WorkerFate::kWorkerError);
}

TEST(ClassifyTest, SignalsAreFaultsKillsOrTheControllersOwn) {
  for (const int signal : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP}) {
    EXPECT_EQ(classify(after(signalled(signal))), WorkerFate::kFault) << signal;
  }
  EXPECT_EQ(classify(after(signalled(SIGKILL))), WorkerFate::kKilled)
      << "SIGKILL from outside: the OOM killer's shape, reported as the fact";
  EXPECT_EQ(classify(after(signalled(SIGTERM))), WorkerFate::kKilled)
      << "a SIGTERM the controller did not send";

  Evidence stopped = after(signalled(SIGTERM));
  stopped.signal_sent = SIGTERM;
  EXPECT_EQ(classify(stopped), WorkerFate::kStoppedByController);

  Evidence killed_stale = after(signalled(SIGKILL));
  killed_stale.signal_sent = SIGKILL;
  killed_stale.stale_when_signalled = true;
  EXPECT_EQ(classify(killed_stale), WorkerFate::kTimedOut);

  Evidence killed_at_deadline = killed_stale;
  killed_at_deadline.stale_when_signalled = false;
  EXPECT_EQ(classify(killed_at_deadline), WorkerFate::kStoppedByController);

  // The controller sent SIGTERM; the worker died of SIGSEGV meanwhile. The
  // fault is the fact; the signal the controller sent is not what killed it.
  Evidence crashed_while_stopping = after(signalled(SIGSEGV));
  crashed_while_stopping.signal_sent = SIGTERM;
  EXPECT_EQ(classify(crashed_while_stopping), WorkerFate::kFault);

  // Exited 0 after the controller asked: the request was honoured, and with
  // the finished record that is a completion, not a stop.
  Evidence honoured = after(exited(0));
  honoured.signal_sent = SIGTERM;
  honoured.finished_seen = true;
  EXPECT_EQ(classify(honoured), WorkerFate::kCompleted);
}

TEST(ClassifyTest, ACorruptChannelOutranksEverythingElse) {
  Evidence corrupt = after(exited(0));
  corrupt.finished_seen = true;
  corrupt.channel_corrupt = true;
  EXPECT_EQ(classify(corrupt), WorkerFate::kChannelCorrupt);
  Evidence corrupt_fault = after(signalled(SIGSEGV));
  corrupt_fault.channel_corrupt = true;
  EXPECT_EQ(classify(corrupt_fault), WorkerFate::kChannelCorrupt);
}

TEST(ClassifyTest, AStatusWordThatIsNotATerminationIsUnrecognised) {
  // Stopped and continued are not terminations and waitpid without WUNTRACED
  // never reports them, so a classifier that sees one says so (F20).
  EXPECT_EQ(classify(after(ExitStatus{0x7f | (SIGSTOP << 8)})), WorkerFate::kUnrecognised);
  EXPECT_EQ(classify(after(ExitStatus{0xffff})), WorkerFate::kUnrecognised) << "continued";
  EXPECT_EQ(classify(after(ExitStatus{0xff})), WorkerFate::kUnrecognised)
      << "a word that matches none of the four shapes";
}

}  // namespace
}  // namespace loadforge::controller
