// SPDX-License-Identifier: GPL-3.0-or-later
//
// The null workload loop: beats on time, stops on time, stops on request,
// and leaves when the controller does -- by hang-up, by staleness, or by a
// controller that died mid-publish.

#include "worker/scheduler.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/worker/control_frames.hpp"
#include "worker/control.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"

namespace loadforge::worker {
namespace {

using loadforge::ipc::Heartbeat;
using loadforge::ipc::HeartbeatRegion;
using loadforge::platform::PipeEnds;
using loadforge::platform::Readiness;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;

/// A controller's view and a worker's loop over one fake: two workers' worth
/// of region (the loop under test uses slot 0; slot 1 stays silent), a control
/// pipe whose write end the test holds as the controller, and clocks that
/// advance 10 ms per read so each tick is 10 ms.
struct Rig {
  FakeSyscalls syscalls;
  std::optional<HeartbeatRegion> region;
  PipeEnds control;
  PipeEnds records;
  std::optional<ipc::RecordWriter> writer;

  Rig() {
    auto created = HeartbeatRegion::create(syscalls, 2);
    EXPECT_TRUE(created.has_value());
    region.emplace(std::move(created).value());
    auto pipe = syscalls.create_pipe();
    EXPECT_TRUE(pipe.has_value());
    control = pipe.value();
    records = syscalls.create_pipe().value();
    writer.emplace(syscalls, records.write_fd);
    syscalls.advance_clocks_per_read(10 * kMs);
  }

  /// A message as the controller frames it, delivered to the worker.
  void send(const ControlMessage& message) {
    syscalls.deliver_to_pipe(control.read_fd, worker_testing::control_frame(message));
  }

  /// The acknowledgements the worker has sent.
  [[nodiscard]] std::vector<ControlAck> answers() const {
    return worker_testing::acknowledgements_in(syscalls.pipe_contents(records.write_fd));
  }

  void controller_beats(std::int64_t timestamp_ns) {
    EXPECT_TRUE(
        region->publish(ipc::kControllerSlot, Heartbeat{timestamp_ns, 1, 1, 1}).has_value());
  }

  Session session(Pacing pacing) { return Session{&*region, 0, control.read_fd, pacing, &*writer}; }

  core::Result<Outcome, WorkerError> run(Pacing pacing) {
    NullWorkload workload(syscalls, session(pacing));
    return workload.run();
  }

  std::optional<Heartbeat> worker_beat() {
    auto observed = region->observe(ipc::worker_slot(0));
    EXPECT_TRUE(observed.has_value());
    return observed.has_value() ? observed.value() : std::nullopt;
  }
};

TEST(SchedulerWordsTest, EveryReasonHasWordsAndTheLostTableIsExact) {
  EXPECT_EQ(describe(StopReason::kDurationElapsed), "the duration elapsed");
  EXPECT_EQ(describe(StopReason::kStopRequested), "the controller asked for a stop");
  EXPECT_EQ(describe(StopReason::kControllerHungUp), "the controller's control pipe hung up");
  EXPECT_EQ(describe(StopReason::kControllerStale), "the controller's heartbeat went stale");
  EXPECT_EQ(describe(StopReason::kControllerUnreadable),
            "the controller's heartbeat could not be read");
  EXPECT_EQ(describe(StopReason::kControlChannelBroken),
            "the controller's control pipe carried something that is not a message");
  EXPECT_EQ(describe(StopReason::kRunnerStopped), "the verification runner stopped the run");
  EXPECT_EQ(describe(static_cast<StopReason>(200)), "unrecognised stop reason");

  EXPECT_FALSE(controller_lost(StopReason::kDurationElapsed));
  EXPECT_FALSE(controller_lost(StopReason::kStopRequested));
  EXPECT_FALSE(controller_lost(StopReason::kRunnerStopped)) << "the controller did not go";
  EXPECT_TRUE(controller_lost(StopReason::kControllerHungUp));
  EXPECT_TRUE(controller_lost(StopReason::kControllerStale));
  EXPECT_TRUE(controller_lost(StopReason::kControllerUnreadable));
  EXPECT_TRUE(controller_lost(StopReason::kControlChannelBroken))
      << "a worker that can no longer be stopped leaves as from a lost controller";

  EXPECT_EQ(describe(WorkerError{"clock", "went backwards"}), "clock: went backwards");
}

TEST(SchedulerWordsTest, TheIntervalInMillisecondsIsAtLeastOneAndNeverWraps) {
  EXPECT_EQ(interval_milliseconds(5'000'000), 5);
  EXPECT_EQ(interval_milliseconds(1'999'999), 1);
  EXPECT_EQ(interval_milliseconds(999'999), 1) << "a wait is never zero";
  EXPECT_EQ(interval_milliseconds(0), 1);
  EXPECT_EQ(interval_milliseconds(-5), 1);
  constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
  EXPECT_EQ(interval_milliseconds(kIntMax * 1'000'000), std::numeric_limits<int>::max());
  // 2^31 ms and 2^32 ms wrapped to negative and to zero in the cast.
  EXPECT_EQ(interval_milliseconds((std::int64_t{1} << 31U) * 1'000'000),
            std::numeric_limits<int>::max());
  EXPECT_EQ(interval_milliseconds((std::int64_t{1} << 32U) * 1'000'000),
            std::numeric_limits<int>::max());
  EXPECT_EQ(interval_milliseconds(std::numeric_limits<std::int64_t>::max()),
            std::numeric_limits<int>::max());
}

TEST(SchedulerWordsTest, EqualityIsFieldByField) {
  const Pacing p{1, 2, 3};
  EXPECT_EQ(p, (Pacing{1, 2, 3}));
  EXPECT_NE(p, (Pacing{9, 2, 3}));
  EXPECT_NE(p, (Pacing{1, 9, 3}));
  EXPECT_NE(p, (Pacing{1, 2, 9}));
  const Outcome o{StopReason::kStopRequested, 4, 5};
  EXPECT_EQ(o, (Outcome{StopReason::kStopRequested, 4, 5}));
  EXPECT_NE(o, (Outcome{StopReason::kDurationElapsed, 4, 5}));
  EXPECT_NE(o, (Outcome{StopReason::kStopRequested, 9, 5}));
  EXPECT_NE(o, (Outcome{StopReason::kStopRequested, 4, 9}));
  const WorkerError e{"a", "b"};
  EXPECT_EQ(e, (WorkerError{"a", "b"}));
  EXPECT_NE(e, (WorkerError{"x", "b"}));
  EXPECT_NE(e, (WorkerError{"a", "x"}));
}

TEST(NullWorkloadTest, BeatsEveryTickAndCompletesWhenTheDurationElapses) {
  Rig rig;
  rig.controller_beats(0);
  // Ticks at 10, 20, 30, 40, 50 ms after the first clock read: the fifth
  // reaches the duration. Four polls were waited, so four ticks of progress.
  auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kDurationElapsed, 4, 50 * kMs}));
  EXPECT_EQ(rig.syscalls.poll_count(), 4);
  EXPECT_EQ(rig.syscalls.poll_timeouts(), (std::vector<int>{10, 10, 10, 10}));
  // The last beat went out before the decision, with the final progress.
  const std::optional<Heartbeat> last = rig.worker_beat();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(*last, (Heartbeat{50 * kMs, 4, NullWorkload::kPhaseRunning, FakeSyscalls::kFakePid}));
  // The control pipe was never read: nothing was written on it.
  EXPECT_EQ(rig.syscalls.read_count(), 0);
}

TEST(NullWorkloadTest, StopsCleanlyWhenTheControllerSendsAStopAndAnswersIt) {
  Rig rig;
  rig.controller_beats(0);
  rig.send({41, Stop{}});
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kStopRequested, 0, 10 * kMs}));
  EXPECT_EQ(rig.syscalls.read_count(), 1) << "the frame was consumed";
  EXPECT_TRUE(rig.worker_beat().has_value()) << "a beat went out before the stop";
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{41, AckStatus::kDone, ""}}));
}

TEST(NullWorkloadTest, EveryOtherCommandIsRefusedInWordsAndTheLoopGoesOn) {
  Rig rig;
  rig.controller_beats(0);
  rig.send({1, Pause{}});
  rig.send({2, Resume{}});
  rig.send({3, SetDuty{500, 1'000'000'000}});
  rig.send({4, Rebind{{0}}});
  rig.send({5, Switch{"compression", 1, 1U << 20U, 0, {}}});
  auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().reason, StopReason::kDurationElapsed) << "none of them was a Stop";
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 5U);
  const char* const names[] = {"pause", "resume", "set duty", "rebind", "switch"};
  for (std::size_t at = 0; at < answers.size(); ++at) {
    EXPECT_EQ(answers[at].sequence, at + 1) << "in the order sent, each once";
    EXPECT_EQ(answers[at].status, AckStatus::kRefused);
    EXPECT_EQ(answers[at].detail, std::string{"the null workload runs no slices: "} + names[at]);
  }
}

TEST(NullWorkloadTest, MessagesAfterAStopAreNotHandled) {
  Rig rig;
  rig.controller_beats(0);
  rig.send({1, Pause{}});
  rig.send({2, Stop{}});
  rig.send({3, Resume{}});
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().reason, StopReason::kStopRequested);
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 2U) << "the Resume after the Stop is left unanswered";
  EXPECT_EQ(answers[1], (ControlAck{2, AckStatus::kDone, ""}));
}

TEST(NullWorkloadTest, APipeThatCarriesNothingLikeAMessageIsAControllerBroken) {
  Rig rig;
  rig.controller_beats(0);
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, std::string(20, 's'));
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControlChannelBroken, 0, 10 * kMs}));
}

TEST(NullWorkloadTest, AnAnswerThatCannotBeSentIsTheControllerGoneOrTheWorkersFailure) {
  {
    Rig rig;
    rig.controller_beats(0);
    rig.send({1, Stop{}});
    ASSERT_TRUE(rig.syscalls.close(rig.records.read_fd).has_value());
    auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().reason, StopReason::kControllerHungUp);
  }
  {
    Rig rig;
    rig.controller_beats(0);
    rig.send({1, Pause{}});
    rig.syscalls.always_fail_write(EIO);
    auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
}

TEST(NullWorkloadTest, LeavesWhenTheControlPipeHangsUpHoweverPollSaysSo) {
  // The writer closing is seen as a hang-up, as an error condition, or as an
  // invalid descriptor, depending on the kernel and the moment; each means
  // the same thing. And a kernel that reports only "readable" for a closed
  // writer is answered by the read returning end of file.
  for (const Readiness readiness :
       {Readiness{false, true, false, false}, Readiness{false, false, true, false},
        Readiness{false, false, false, true}}) {
    Rig rig;
    rig.controller_beats(0);
    rig.syscalls.script_readiness(rig.control.read_fd, readiness);
    auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
    EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControllerHungUp, 0, 10 * kMs}));
  }
  Rig rig;
  rig.controller_beats(0);
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  auto by_model = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(by_model.has_value());
  EXPECT_EQ(by_model.value().reason, StopReason::kControllerHungUp);

  Rig eof;
  eof.controller_beats(0);
  ASSERT_TRUE(eof.syscalls.close(eof.control.write_fd).has_value());
  eof.syscalls.script_readiness(eof.control.read_fd, Readiness{true, false, false, false});
  auto by_read = eof.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(by_read.has_value());
  EXPECT_EQ(by_read.value().reason, StopReason::kControllerHungUp);
  EXPECT_EQ(eof.syscalls.read_count(), 1);
}

TEST(NullWorkloadTest, LeavesWhenTheControllersBeatGoesStale) {
  Rig rig;
  rig.controller_beats(0);
  // Age at tick k is k * 10 ms; the allowance is 30 ms, strictly; tick 4 is
  // the first past it.
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 30 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControllerStale, 3, 40 * kMs}));
}

TEST(NullWorkloadTest, AControllerThatNeverBeatIsStaleOnceTheAllowanceHasPassed) {
  Rig rig;
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 30 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControllerStale, 3, 40 * kMs}));
  // Within the allowance the worker gives it time: a short run completes.
  Rig patient;
  auto completed = patient.run(Pacing{20 * kMs, 10 * kMs, 30 * kMs});
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed.value(), (Outcome{StopReason::kDurationElapsed, 1, 20 * kMs}));
}

TEST(NullWorkloadTest, LeavesWhenTheControllerDiedInsideAPublish) {
  Rig rig;
  // The controller's slot with its sequence left odd: a writer mid-update
  // that will never finish. Written through a second mapping of the file.
  auto mapped = rig.syscalls.map_shared(rig.region->fd(), rig.region->bytes());
  ASSERT_TRUE(mapped.has_value());
  const std::uint64_t odd = 1;
  std::memcpy(static_cast<std::byte*>(mapped.value()) + HeartbeatRegion::kHeaderBytes, &odd,
              sizeof odd);
  // Unreadable is a reason to leave only once it has lasted longer than the
  // allowance (100 ms, strictly): a controller descheduled between the two
  // halves of a publish is unreadable too, for a while. The first reading
  // was at 0, so the tick at 110 ms leaves, after ten polls.
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControllerUnreadable, 10, 110 * kMs}));
  EXPECT_TRUE(rig.syscalls.unmap(mapped.value(), rig.region->bytes()).has_value());
}

TEST(NullWorkloadTest, AControllerSlotUnreadableForLessThanTheAllowanceIsWaitedOut) {
  // The same odd sequence, for a run shorter than the allowance: the worker
  // keeps working and completes. One torn read is no evidence of a death;
  // the first version left on it, and under TSan with every core busy a
  // healthy controller descheduled mid-publish made its workers exit 124.
  // Once on a clock that starts at zero, and once on one that starts where a
  // real machine's does, well past it: how long the slot has been unreadable
  // is measured from the last reading, never from the clock's own zero.
  for (const std::int64_t base_s : {std::int64_t{0}, std::int64_t{86'400}}) {
    Rig rig;
    for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
      rig.syscalls.set_clock(clock_id, platform::TimeSpec{base_s, 0});
    }
    auto mapped = rig.syscalls.map_shared(rig.region->fd(), rig.region->bytes());
    ASSERT_TRUE(mapped.has_value());
    const std::uint64_t odd = 1;
    std::memcpy(static_cast<std::byte*>(mapped.value()) + HeartbeatRegion::kHeaderBytes, &odd,
                sizeof odd);
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
    EXPECT_EQ(outcome.value(), (Outcome{StopReason::kDurationElapsed, 4, 50 * kMs})) << base_s;
    EXPECT_TRUE(rig.syscalls.unmap(mapped.value(), rig.region->bytes()).has_value());
  }
}

TEST(NullWorkloadTest, EveryFailureNamesItsStage) {
  {
    Rig rig;
    rig.syscalls.fail_clock(CLOCK_MONOTONIC_RAW, EINVAL);
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_FALSE(rig.worker_beat().has_value()) << "no beat before the first reading";
  }
  {
    Rig rig;
    rig.controller_beats(0);
    rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC, 3, EIO);  // The second tick's reading.
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_EQ(rig.syscalls.poll_count(), 1) << "one tick ran before the clock failed";
  }
  {
    Rig rig;
    rig.controller_beats(0);
    // A clock running backwards: every reading ten milliseconds earlier than
    // the last, from a base far enough from zero that the readings themselves
    // stay well formed.
    for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
      rig.syscalls.set_clock(clock_id, platform::TimeSpec{100, 0});
    }
    rig.syscalls.advance_clocks_per_read(-10 * kMs);
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_NE(outcome.error().detail.find("backwards"), std::string::npos)
        << outcome.error().detail;
  }
  {
    Rig rig;
    Session session = rig.session(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    session.slot = 5;  // The region has two worker slots.
    NullWorkload workload(rig.syscalls, session);
    auto outcome = workload.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "heartbeat");
    EXPECT_EQ(outcome.error().detail, "no such slot: slot 6, expected 3");
  }
  {
    Rig rig;
    rig.controller_beats(0);
    rig.syscalls.fail_poll(EBADF);
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "control pipe");
    EXPECT_EQ(outcome.error().detail, "poll(1 descriptors): Bad file descriptor (errno 9)");
  }
  {
    Rig rig;
    rig.controller_beats(0);
    rig.syscalls.script_readiness(rig.control.read_fd, Readiness{true, false, false, false});
    rig.syscalls.always_fail_read(EIO);
    auto outcome = rig.run(Pacing{50 * kMs, 10 * kMs, 100 * kMs});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "control pipe");
    EXPECT_NE(outcome.error().detail.find("read("), std::string::npos) << outcome.error().detail;
  }
}

}  // namespace
}  // namespace loadforge::worker
