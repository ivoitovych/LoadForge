// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/control_channel.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/control.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::worker {

Delivery deliver(ipc::RecordWriter& records, std::uint16_t type,
                 const std::vector<std::byte>& bytes) {
  Delivery delivery;
  auto written = records.send(type, bytes);
  if (written.has_value()) {
    return delivery;
  }
  if (written.error().cause.number == EPIPE) {
    delivery.controller_gone = true;
    return delivery;
  }
  delivery.failure = WorkerError{"record channel", ipc::describe(written.error())};
  return delivery;
}

core::Result<ControlLook, WorkerError> ControlChannel::look(int wait_ms) {
  ControlLook found;
  const std::vector<int> watched{fd_};
  auto ready = syscalls_->poll_readable(watched, wait_ms);
  if (!ready.has_value()) {
    return WorkerError{"control pipe", describe(ready.error())};
  }
  const platform::Readiness& control = ready.value().front();
  if (!control.readable) {
    // Nothing to read: the interval passed, or the far end is gone with
    // nothing left in the pipe.
    if (control.hung_up || control.error || control.invalid) {
      found.lost = StopReason::kControllerHungUp;
    }
    return found;
  }
  auto drained = source_.drain();
  if (!drained.has_value()) {
    const std::optional<ipc::FrameError>& frame = drained.error().frame;
    if (!frame.has_value()) {
      return WorkerError{"control pipe", ipc::describe(drained.error())};
    }
    // The stream ended inside a frame: the controller died writing it.
    found.lost = frame->fault == ipc::FrameFault::kTruncated ? StopReason::kControllerHungUp
                                                             : StopReason::kControlChannelBroken;
    return found;
  }
  for (const ipc::Record& record : drained.value().records) {
    if (record.type != kControlFrameType) {
      found.lost = StopReason::kControlChannelBroken;
      return found;
    }
    auto message = decode_message(record.payload);
    if (!message.has_value()) {
      found.lost = StopReason::kControlChannelBroken;
      return found;
    }
    found.messages.push_back(std::move(message).value());
  }
  if (drained.value().end_of_stream) {
    found.lost = StopReason::kControllerHungUp;
  }
  return found;
}

Delivery ControlChannel::answer(std::uint32_t sequence, AckStatus status, std::string detail) {
  ControlAck ack;
  ack.sequence = sequence;
  ack.status = status;
  ack.detail = std::move(detail);
  if (ack.detail.size() > verification::kMaxTextBytes) {
    ack.detail.resize(verification::kMaxTextBytes);
  }
  return deliver(*answers_, kControlAckFrameType, encode(ack));
}

}  // namespace loadforge::worker
