// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classification table: every row of the misbehaviour catalogue is one
// Evidence and one fate, and the fates that must never be confused are not.

#include "controller/supervision.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <optional>
#include <string>

#include "core/result.hpp"
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
  EXPECT_EQ(describe(WorkerFate::kOomKilled), "killed by the OOM killer");
  EXPECT_EQ(describe(WorkerFate::kStoppedByController), "stopped by the controller");
  EXPECT_EQ(describe(WorkerFate::kTimedOut), "timed out");
  EXPECT_EQ(describe(WorkerFate::kChannelCorrupt), "record channel corrupt");
  EXPECT_EQ(describe(WorkerFate::kTruncatedEvidence), "evidence truncated");
  EXPECT_EQ(describe(WorkerFate::kUnrecognised), "unrecognised termination");
  EXPECT_EQ(describe(static_cast<WorkerFate>(200)), "unrecognised fate");

  EXPECT_TRUE(is_clean(WorkerFate::kCompleted));
  for (const WorkerFate fate :
       {WorkerFate::kWorkerError, WorkerFate::kStartupFailed, WorkerFate::kControllerLostReported,
        WorkerFate::kFault, WorkerFate::kKilled, WorkerFate::kOomKilled,
        WorkerFate::kStoppedByController, WorkerFate::kTimedOut, WorkerFate::kChannelCorrupt,
        WorkerFate::kTruncatedEvidence, WorkerFate::kUnrecognised}) {
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
  b.signal_attempted = SIGTERM;
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
  b = a;
  b.oom_kill_recorded = true;
  EXPECT_NE(a, b);
  b = a;
  b.oom_inconclusive = true;
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
  stopped.signal_attempted = SIGTERM;
  EXPECT_EQ(classify(stopped), WorkerFate::kStoppedByController);

  Evidence killed_stale = after(signalled(SIGKILL));
  killed_stale.signal_attempted = SIGKILL;
  killed_stale.stale_when_signalled = true;
  EXPECT_EQ(classify(killed_stale), WorkerFate::kTimedOut);

  Evidence killed_at_deadline = killed_stale;
  killed_at_deadline.stale_when_signalled = false;
  EXPECT_EQ(classify(killed_at_deadline), WorkerFate::kStoppedByController);

  // The controller sent SIGTERM; the worker died of SIGSEGV meanwhile. The
  // fault is the fact; the signal the controller sent is not what killed it.
  Evidence crashed_while_stopping = after(signalled(SIGSEGV));
  crashed_while_stopping.signal_attempted = SIGTERM;
  EXPECT_EQ(classify(crashed_while_stopping), WorkerFate::kFault);

  // Exited 0 after the controller asked: the request was honoured, and with
  // the finished record that is a completion, not a stop.
  Evidence honoured = after(exited(0));
  honoured.signal_attempted = SIGTERM;
  honoured.finished_seen = true;
  EXPECT_EQ(classify(honoured), WorkerFate::kCompleted);
}

// --- the OOM killer, told from a fault and from a plain kill ------------------------

/// A SIGKILL the controller did not send, with the kernel's record behind it.
Evidence recorded(ExitStatus status) {
  Evidence evidence = after(status);
  evidence.oom_kill_recorded = true;
  return evidence;
}

TEST(OomClassifyTest, ASigkillNobodyHereSentIsAnOomKillOnlyWithTheKernelsRecord) {
  EXPECT_EQ(classify(recorded(signalled(SIGKILL))), WorkerFate::kOomKilled);
  EXPECT_EQ(classify(after(signalled(SIGKILL))), WorkerFate::kKilled)
      << "the same death with no record is the fact, not the inference";

  // The controller had asked it to stop with SIGTERM; the SIGKILL was not its.
  Evidence terminated = recorded(signalled(SIGKILL));
  terminated.signal_attempted = SIGTERM;
  EXPECT_EQ(classify(terminated), WorkerFate::kOomKilled);
}

TEST(OomClassifyTest, TheRecordExplainsNoOtherDeath) {
  // The OOM killer sends SIGKILL and nothing else.
  EXPECT_EQ(classify(recorded(signalled(SIGTERM))), WorkerFate::kKilled);
  for (const int signal : {SIGSEGV, SIGBUS, SIGABRT}) {
    EXPECT_EQ(classify(recorded(signalled(signal))), WorkerFate::kFault)
        << "a fault stays a fault: " << signal;
  }
  // A SIGKILL the controller attempted does not outrank the record: kill(2)
  // succeeds on a zombie, so the attempt proves nothing about the cause. The
  // rise in the count explains the death (EXT-01).
  Evidence escalated = recorded(signalled(SIGKILL));
  escalated.signal_attempted = SIGKILL;
  EXPECT_EQ(classify(escalated), WorkerFate::kOomKilled);
  escalated.stale_when_signalled = true;
  EXPECT_EQ(classify(escalated), WorkerFate::kOomKilled);
  // An exit is an exit.
  EXPECT_EQ(classify(recorded(exited(1))), WorkerFate::kWorkerError);
  Evidence finished = recorded(exited(0));
  finished.finished_seen = true;
  EXPECT_EQ(classify(finished), WorkerFate::kCompleted);
  // And garbage on the channel outranks the record as it outranks everything.
  Evidence corrupt = recorded(signalled(SIGKILL));
  corrupt.channel_corrupt = true;
  EXPECT_EQ(classify(corrupt), WorkerFate::kChannelCorrupt);
  EXPECT_EQ(classify(recorded(ExitStatus{0xff})), WorkerFate::kUnrecognised);
}

TEST(OomCandidateTest, AnySigkillDeathOnAnIntactChannelIncludingOneTheControllerAttempted) {
  EXPECT_TRUE(oom_candidate(after(signalled(SIGKILL))));
  Evidence after_term = after(signalled(SIGKILL));
  after_term.signal_attempted = SIGTERM;
  EXPECT_TRUE(oom_candidate(after_term)) << "the controller sent SIGTERM, not this SIGKILL";

  Evidence sent = after(signalled(SIGKILL));
  sent.signal_attempted = SIGKILL;
  EXPECT_TRUE(oom_candidate(sent))
      << "the controller's attempted SIGKILL does not take the death out of the count's reach";
  Evidence corrupt = after(signalled(SIGKILL));
  corrupt.channel_corrupt = true;
  EXPECT_FALSE(oom_candidate(corrupt));
  EXPECT_FALSE(oom_candidate(after(signalled(SIGTERM))));
  EXPECT_FALSE(oom_candidate(after(signalled(SIGSEGV))));
  EXPECT_FALSE(oom_candidate(after(exited(9)))) << "exit code 9 is not signal 9";
  EXPECT_FALSE(oom_candidate(after(ExitStatus{0x7f | (SIGKILL << 8)})))
      << "stopped, with SIGKILL's number where the signal goes";
}

using Count = core::Result<std::uint64_t, std::string>;
constexpr std::string_view kScope = "/sys/fs/cgroup/run.scope/memory.events";

// --- a controller SIGKILL is an attempt, not a cause (EXT-01) -------------------------

/// The controller attempted SIGKILL and the worker was reaped dead of SIGKILL.
Evidence attempted_kill(bool stale) {
  Evidence evidence = after(signalled(SIGKILL));
  evidence.signal_attempted = SIGKILL;
  evidence.stale_when_signalled = stale;
  return evidence;
}

TEST(AttemptedKillTest, WithTheCountReadAndUnmovedItIsTheControllersStop) {
  // Nothing consulted, or the count read and not moved: the attempt is the
  // best-supported reading and is reported as before.
  EXPECT_EQ(classify(attempted_kill(false)), WorkerFate::kStoppedByController);
  EXPECT_EQ(classify(attempted_kill(true)), WorkerFate::kTimedOut);
}

TEST(AttemptedKillTest, ARiseInTheCountOutranksTheAttempt) {
  Evidence stop = attempted_kill(false);
  stop.oom_kill_recorded = true;
  EXPECT_EQ(classify(stop), WorkerFate::kOomKilled);
  Evidence timeout = attempted_kill(true);
  timeout.oom_kill_recorded = true;
  EXPECT_EQ(classify(timeout), WorkerFate::kOomKilled);
}

TEST(AttemptedKillTest, ACountThatCannotAccountForTheDeathLeavesTheProvenanceAmbiguous) {
  // The count was consulted and neither explains the death nor rules an OOM
  // kill out. The attempt and an OOM kill (or another SIGKILL) cannot be told
  // apart, and the verdict is the neutral one: killed.
  for (const bool stale : {false, true}) {
    Evidence ambiguous = attempted_kill(stale);
    ambiguous.oom_inconclusive = true;
    EXPECT_EQ(classify(ambiguous), WorkerFate::kKilled) << stale;
  }
}

TEST(AttemptedKillTest, OnlyASigkillDeathMatchingASigkillAttemptIsAmbiguous) {
  // A SIGTERM attempt that killed by SIGTERM is the controller's, whatever the
  // count: the OOM killer sends SIGKILL and nothing else.
  Evidence term = after(signalled(SIGTERM));
  term.signal_attempted = SIGTERM;
  term.oom_inconclusive = true;
  EXPECT_EQ(classify(term), WorkerFate::kStoppedByController);
  term.oom_kill_recorded = true;
  EXPECT_EQ(classify(term), WorkerFate::kStoppedByController);
  // A worker that was sent SIGKILL and died of a fault is a fault.
  Evidence crashed = after(signalled(SIGSEGV));
  crashed.signal_attempted = SIGKILL;
  crashed.oom_inconclusive = true;
  EXPECT_EQ(classify(crashed), WorkerFate::kFault);
}

TEST(OomFindingTest, OnlyACountReadAtBothEndsAndUnmovedRulesAnOomKillOut) {
  EXPECT_TRUE(judge_oom_kills(Count{7}, Count{7}, 1, "scope").ruled_out);
  EXPECT_FALSE(judge_oom_kills(Count{7}, Count{7}, 1, "scope").attributed);
  EXPECT_FALSE(judge_oom_kills(Count{4}, Count{5}, 1, "scope").ruled_out);
  EXPECT_FALSE(judge_oom_kills(Count{0}, Count{1}, 2, "scope").ruled_out);
  EXPECT_FALSE(judge_oom_kills(Count{5}, Count{2}, 1, "scope").ruled_out);
  EXPECT_FALSE(judge_oom_kills(std::string{"no"}, Count{2}, 1, "scope").ruled_out);
  EXPECT_FALSE(judge_oom_kills(Count{2}, std::string{"no"}, 1, "scope").ruled_out);
}

TEST(JudgeOomTest, EnoughKillsCountedAttributesEveryCandidate) {
  const OomFinding one = judge_oom_kills(Count{4}, Count{5}, 1, kScope);
  EXPECT_TRUE(one.attributed);
  EXPECT_EQ(one.note,
            "OOM kill: /sys/fs/cgroup/run.scope/memory.events counted 1 during the run, one or "
            "more for each of the 1 worker(s) killed by a SIGKILL");
  EXPECT_TRUE(judge_oom_kills(Count{0}, Count{2}, 2, kScope).attributed) << "exactly as many";
  EXPECT_TRUE(judge_oom_kills(Count{0}, Count{3}, 2, kScope).attributed)
      << "more: something else in the cgroup was killed too, and each worker still has one";
}

TEST(JudgeOomTest, NoKillCountedLeavesAKill) {
  const OomFinding none = judge_oom_kills(Count{7}, Count{7}, 1, kScope);
  EXPECT_FALSE(none.attributed);
  EXPECT_EQ(none.note,
            "not attributed to the OOM killer: /sys/fs/cgroup/run.scope/memory.events counted no "
            "OOM kill during the run");
}

TEST(JudgeOomTest, FewerKillsThanCandidatesAttributesNoneBecauseACountDoesNotSayWhich) {
  const OomFinding few = judge_oom_kills(Count{0}, Count{1}, 2, kScope);
  EXPECT_FALSE(few.attributed);
  EXPECT_EQ(few.note,
            "not attributed to the OOM killer: /sys/fs/cgroup/run.scope/memory.events counted 1 "
            "OOM kill(s) during the run for 2 workers killed by a SIGKILL, and a count does not "
            "say which");
}

TEST(JudgeOomTest, ACountThatWentBackIsNotTheSameCount) {
  const OomFinding back = judge_oom_kills(Count{5}, Count{2}, 1, kScope);
  EXPECT_FALSE(back.attributed);
  EXPECT_EQ(back.note,
            "not attributed to the OOM killer: /sys/fs/cgroup/run.scope/memory.events went from 5 "
            "to 2, so it is not the count that was read before the run");
}

TEST(JudgeOomTest, ACountThatCouldNotBeReadAtEitherEndAttributesNothingAndSaysWhy) {
  const OomFinding no_before =
      judge_oom_kills(Count{std::string{"x: not permitted (y)"}}, Count{9}, 1, kScope);
  EXPECT_FALSE(no_before.attributed);
  EXPECT_EQ(no_before.note,
            "not attributed to the OOM killer: the OOM-kill count could not be read before the "
            "run: x: not permitted (y)");
  const OomFinding no_after = judge_oom_kills(
      Count{0}, Count{std::string{"x: gone since it was last read (z)"}}, 1, kScope);
  EXPECT_FALSE(no_after.attributed);
  EXPECT_EQ(no_after.note,
            "not attributed to the OOM killer: the OOM-kill count could not be read after the "
            "run: x: gone since it was last read (z)");
  EXPECT_FALSE(judge_oom_kills(Count{std::string{"a"}}, Count{std::string{"b"}}, 1, kScope)
                   .note.ends_with("b"))
      << "the earlier failure is the one reported";
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
