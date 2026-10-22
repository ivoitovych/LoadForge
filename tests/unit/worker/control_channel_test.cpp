// SPDX-License-Identifier: GPL-3.0-or-later
//
// The worker's end of the control pipe: messages read as frames, a controller
// that has gone, a pipe that carries something that is not a message, and the
// acknowledgements sent back.

#include "worker/control_channel.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "support/fake_syscalls.hpp"
#include "unit/worker/control_frames.hpp"
#include "worker/control.hpp"
#include "worker/ipc/record.hpp"

namespace loadforge::worker {
namespace {

using loadforge::platform::PipeEnds;
using loadforge::platform::Readiness;
using loadforge::testing::FakeSyscalls;

using worker_testing::control_frame;
using worker_testing::text_of;

/// A worker's end over the fake: a control pipe the test writes into and a
/// record pipe it reads the answers from.
struct Rig {
  FakeSyscalls syscalls;
  PipeEnds control;
  PipeEnds records;
  ipc::RecordWriter writer;
  ControlChannel channel;

  Rig()
      : control(syscalls.create_pipe().value()),
        records(syscalls.create_pipe().value()),
        writer(syscalls, records.write_fd),
        channel(syscalls, control.read_fd, writer) {}

  void send(const ControlMessage& message) {
    syscalls.deliver_to_pipe(control.read_fd, control_frame(message));
  }

  /// The acknowledgements the worker has sent, decoded.
  [[nodiscard]] std::vector<ControlAck> answers() const {
    return worker_testing::acknowledgements_in(syscalls.pipe_contents(records.write_fd));
  }
};

TEST(ControlChannelLook, NothingToReadIsAnEmptyLookAfterTheWait) {
  Rig rig;
  auto looked = rig.channel.look(25);
  ASSERT_TRUE(looked.has_value());
  EXPECT_TRUE(looked.value().messages.empty());
  EXPECT_FALSE(looked.value().lost.has_value());
  EXPECT_EQ(rig.syscalls.poll_timeouts(), (std::vector<int>{25}));
  EXPECT_EQ(rig.syscalls.read_count(), 0) << "an empty pipe is not read";
}

TEST(ControlChannelLook, MessagesComeBackInTheOrderTheyWereSent) {
  Rig rig;
  rig.send({1, Pause{}});
  rig.send({2, SetDuty{250, 1'000'000'000}});
  rig.send({3, Switch{"fft", 5, 1U << 20U, 0, {1, 1}}});
  auto looked = rig.channel.look(0);
  ASSERT_TRUE(looked.has_value());
  ASSERT_EQ(looked.value().messages.size(), 3U);
  EXPECT_EQ(looked.value().messages[0], (ControlMessage{1, Pause{}}));
  EXPECT_EQ(looked.value().messages[1], (ControlMessage{2, SetDuty{250, 1'000'000'000}}));
  EXPECT_EQ(looked.value().messages[2],
            (ControlMessage{3, Switch{"fft", 5, 1U << 20U, 0, {1, 1}}}));
  EXPECT_FALSE(looked.value().lost.has_value());
}

TEST(ControlChannelLook, AMessageThatArrivesInPiecesIsHandledWhenItIsWhole) {
  Rig rig;
  rig.syscalls.limit_pipe_reads(10);
  rig.send({1, Resume{}});
  auto first = rig.channel.look(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first.value().messages.empty()) << "ten bytes of a twenty-two byte frame";
  auto second = rig.channel.look(0);
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second.value().messages.empty());
  auto third = rig.channel.look(0);
  ASSERT_TRUE(third.has_value());
  ASSERT_EQ(third.value().messages.size(), 1U);
  EXPECT_EQ(third.value().messages[0], (ControlMessage{1, Resume{}}));
}

TEST(ControlChannelLook, AControllerThatClosedTheFarEndHasGoneHoweverPollSaysSo) {
  for (const Readiness readiness :
       {Readiness{false, true, false, false}, Readiness{false, false, true, false},
        Readiness{false, false, false, true}}) {
    Rig rig;
    rig.syscalls.script_readiness(rig.control.read_fd, readiness);
    auto looked = rig.channel.look(0);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(looked.value().lost, StopReason::kControllerHungUp);
    EXPECT_EQ(rig.syscalls.read_count(), 0) << "a pipe poll calls dead is not read";
  }
  // Readable, then end of file on the read: the writer is gone.
  Rig eof;
  ASSERT_TRUE(eof.syscalls.close(eof.control.write_fd).has_value());
  eof.syscalls.script_readiness(eof.control.read_fd, Readiness{true, false, false, false});
  auto looked = eof.channel.look(0);
  ASSERT_TRUE(looked.has_value());
  EXPECT_EQ(looked.value().lost, StopReason::kControllerHungUp);
}

TEST(ControlChannelLook, AStopAndThenTheCloseAreBothSeenTheStopFirst) {
  Rig rig;
  rig.send({9, Stop{}});
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  rig.syscalls.script_readiness(rig.control.read_fd, Readiness{true, true, false, false});
  auto looked = rig.channel.look(0);
  ASSERT_TRUE(looked.has_value());
  ASSERT_EQ(looked.value().messages.size(), 1U);
  EXPECT_EQ(looked.value().messages[0], (ControlMessage{9, Stop{}}));
  EXPECT_FALSE(looked.value().lost.has_value())
      << "the data is read first; the end comes next time";
  auto next = rig.channel.look(0);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next.value().lost, StopReason::kControllerHungUp);
}

TEST(ControlChannelLook, AControllerThatDiedInsideAFrameHasHungUp) {
  Rig rig;
  const std::string whole = control_frame({1, Pause{}});
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, whole.substr(0, whole.size() - 3));
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  auto first = rig.channel.look(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first.value().messages.empty());
  auto second = rig.channel.look(0);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value().lost, StopReason::kControllerHungUp)
      << "a stream that ends inside a frame is a writer that died writing it";
}

TEST(ControlChannelLook, ALookThatReadsTheEndOfAStreamInsideAFrameIsTheControllerHungUp) {
  Rig rig;
  const std::string whole = control_frame({1, Pause{}});
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, whole.substr(0, whole.size() - 3));
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  ASSERT_TRUE(rig.channel.look(0).has_value());
  // Poll says readable, as it does for a pipe with data; the read finds the end.
  rig.syscalls.script_readiness(rig.control.read_fd, Readiness{true, true, false, false});
  auto second = rig.channel.look(0);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value().lost, StopReason::kControllerHungUp);
}

TEST(ControlChannelLook, APipeThatCarriesNothingLikeAMessageBreaksTheChannel) {
  {
    Rig rig;  // A byte of the old kind: not a frame at all.
    rig.syscalls.deliver_to_pipe(rig.control.read_fd, std::string(16, 's'));
    auto looked = rig.channel.look(0);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(looked.value().lost, StopReason::kControlChannelBroken);
  }
  {
    Rig rig;  // A frame that verifies and is not a control frame.
    rig.syscalls.deliver_to_pipe(
        rig.control.read_fd,
        text_of(ipc::encode(static_cast<std::uint16_t>(ipc::RecordType::kStarted), {}).value()));
    auto looked = rig.channel.look(0);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(looked.value().lost, StopReason::kControlChannelBroken);
  }
  {
    Rig rig;  // A control frame whose payload is no message.
    const std::vector<std::byte> junk{std::byte{1}, std::byte{9}, std::byte{0}};
    rig.syscalls.deliver_to_pipe(rig.control.read_fd,
                                 text_of(ipc::encode(kControlFrameType, junk).value()));
    auto looked = rig.channel.look(0);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(looked.value().lost, StopReason::kControlChannelBroken);
  }
}

TEST(ControlChannelLook, MessagesBeforeAnythingBrokenAreStillHandled) {
  Rig rig;
  rig.send({1, Pause{}});
  rig.send({2, Resume{}});
  const std::vector<std::byte> junk{std::byte{0xFF}};
  rig.syscalls.deliver_to_pipe(rig.control.read_fd,
                               text_of(ipc::encode(kControlFrameType, junk).value()));
  auto looked = rig.channel.look(0);
  ASSERT_TRUE(looked.has_value());
  EXPECT_EQ(looked.value().messages.size(), 2U);
  EXPECT_EQ(looked.value().lost, StopReason::kControlChannelBroken);
}

TEST(ControlChannelLook, APipeThatFailsIsTheWorkersFailureAndNamesTheControlPipe) {
  {
    Rig rig;
    rig.syscalls.fail_poll(EBADF);
    auto looked = rig.channel.look(0);
    ASSERT_FALSE(looked.has_value());
    EXPECT_EQ(looked.error().stage, "control pipe");
    EXPECT_NE(looked.error().detail.find("poll("), std::string::npos);
  }
  {
    Rig rig;
    rig.syscalls.script_readiness(rig.control.read_fd, Readiness{true, false, false, false});
    rig.syscalls.always_fail_read(EIO);
    // The pipe is a pipe in the fake, so a read failure has to come from the
    // read the channel makes: scripted readiness with an empty pipe gives EAGAIN.
    auto looked = rig.channel.look(0);
    ASSERT_FALSE(looked.has_value());
    EXPECT_EQ(looked.error().stage, "control pipe");
    EXPECT_NE(looked.error().detail.find("read("), std::string::npos) << looked.error().detail;
  }
}

TEST(ControlChannelAnswer, AnAcknowledgementIsAFrameOnTheRecordPipeWithTheSequenceItAnswers) {
  Rig rig;
  EXPECT_FALSE(rig.channel.answer(7, AckStatus::kDone, "").controller_gone);
  EXPECT_FALSE(rig.channel.answer(8, AckStatus::kRefused, "CPU 9 is not in this cpuset").failure);
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 2U);
  EXPECT_EQ(answers[0], (ControlAck{7, AckStatus::kDone, ""}));
  EXPECT_EQ(answers[1], (ControlAck{8, AckStatus::kRefused, "CPU 9 is not in this cpuset"}));
}

TEST(ControlChannelAnswer, AReasonLongerThanAnAcknowledgementHoldsIsCutNotRefused) {
  Rig rig;
  const Delivery sent = rig.channel.answer(1, AckStatus::kRefused,
                                           std::string(verification::kMaxTextBytes + 50, 'x'));
  EXPECT_FALSE(sent.failure.has_value());
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 1U);
  EXPECT_EQ(answers[0].detail.size(), verification::kMaxTextBytes);
}

TEST(ControlChannelAnswer, ARecordPipeWithNoReaderIsTheControllerGoneAnythingElseIsTheWorkers) {
  {
    Rig rig;
    ASSERT_TRUE(rig.syscalls.close(rig.records.read_fd).has_value());
    const Delivery sent = rig.channel.answer(1, AckStatus::kDone, "");
    EXPECT_TRUE(sent.controller_gone);
    EXPECT_FALSE(sent.failure.has_value());
  }
  {
    Rig rig;
    rig.syscalls.always_fail_write(EIO);
    const Delivery sent = rig.channel.answer(1, AckStatus::kDone, "");
    EXPECT_FALSE(sent.controller_gone);
    ASSERT_TRUE(sent.failure.has_value());
    EXPECT_EQ(sent.failure->stage, "record channel");
    EXPECT_NE(sent.failure->detail.find("Input/output error"), std::string::npos);
  }
}

}  // namespace
}  // namespace loadforge::worker
