// SPDX-License-Identifier: GPL-3.0-or-later
//
// The null workload loop: beats on time, stops on time, stops on request,
// and leaves when the controller does -- by hang-up, by staleness, or by a
// controller that died mid-publish.

#include "worker/scheduler.hpp"

#include <gtest/gtest.h>
#include <time.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"

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

  Rig() {
    auto created = HeartbeatRegion::create(syscalls, 2);
    EXPECT_TRUE(created.has_value());
    region.emplace(std::move(created).value());
    auto pipe = syscalls.create_pipe();
    EXPECT_TRUE(pipe.has_value());
    control = pipe.value();
    syscalls.advance_clocks_per_read(10 * kMs);
  }

  void controller_beats(std::int64_t timestamp_ns) {
    EXPECT_TRUE(
        region->publish(ipc::kControllerSlot, Heartbeat{timestamp_ns, 1, 1, 1}).has_value());
  }

  Session session(Pacing pacing) { return Session{&*region, 0, control.read_fd, pacing}; }

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
  EXPECT_EQ(describe(static_cast<StopReason>(200)), "unrecognised stop reason");

  EXPECT_FALSE(controller_lost(StopReason::kDurationElapsed));
  EXPECT_FALSE(controller_lost(StopReason::kStopRequested));
  EXPECT_TRUE(controller_lost(StopReason::kControllerHungUp));
  EXPECT_TRUE(controller_lost(StopReason::kControllerStale));
  EXPECT_TRUE(controller_lost(StopReason::kControllerUnreadable));

  EXPECT_EQ(describe(WorkerError{"clock", "went backwards"}), "clock: went backwards");
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

TEST(NullWorkloadTest, StopsCleanlyWhenTheControllerWritesOnTheControlPipe) {
  Rig rig;
  rig.controller_beats(0);
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, "s");
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kStopRequested, 0, 10 * kMs}));
  EXPECT_EQ(rig.syscalls.read_count(), 1) << "the byte was consumed";
  EXPECT_TRUE(rig.worker_beat().has_value()) << "a beat went out before the stop";
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
  auto outcome = rig.run(Pacing{1000 * kMs, 10 * kMs, 100 * kMs});
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value(), (Outcome{StopReason::kControllerUnreadable, 0, 10 * kMs}));
  EXPECT_TRUE(rig.syscalls.unmap(mapped.value(), rig.region->bytes()).has_value());
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
