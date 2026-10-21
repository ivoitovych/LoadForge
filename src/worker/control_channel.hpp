// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_CONTROL_CHANNEL_HPP
#define LOADFORGE_WORKER_CONTROL_CHANNEL_HPP

// The worker's end of the control pipe and its answers (worker/control.hpp).
//
// Up to M5 the pipe carried one byte, meaning stop. It now carries ipc frames
// of type kControlFrameType, each a ControlMessage, and the worker answers
// each, in the order it handled them, on the record pipe with a frame of type
// kControlAckFrameType. The byte is gone on both sides in the same change.
//
// WHAT A LOOK FINDS
// -----------------
// The messages that arrived, in order, and whether the controller is gone. The
// two come together on purpose: a controller that wrote Stop and closed the
// pipe has both said stop and gone, and the stop is handled first, so the
// worker finishes and reports instead of leaving as if abandoned.
//
// A pipe that carries anything that is not a message -- a frame that does not
// verify, a frame of another type, a payload that does not decode -- is the
// controller broken, not a controller to obey: the framing never resynchronises
// (ipc/record.hpp), so the next Stop could never be read, and a worker that
// can no longer be stopped must not go on. It leaves as a lost controller
// (StopReason::kControlChannelBroken). A stream that ends inside a frame is the
// controller having died mid-write: a hang-up.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "worker/control.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::worker {

/// What sending one frame to the controller came to: sent, the controller
/// gone (EPIPE says the read end has no holder), or a failure that is the
/// worker's own.
struct Delivery {
  bool controller_gone = false;
  std::optional<WorkerError> failure;
};

/// Sends a frame on the record pipe and says how it went.
[[nodiscard]] Delivery deliver(ipc::RecordWriter& records, std::uint16_t type,
                               const std::vector<std::byte>& bytes);

/// What one look at the control pipe found.
struct ControlLook {
  std::vector<ControlMessage> messages;
  std::optional<StopReason> lost;
};

class ControlChannel {
 public:
  ControlChannel(platform::Syscalls& syscalls, int control_fd, ipc::RecordWriter& answers) noexcept
      : syscalls_(&syscalls), fd_(control_fd), source_(syscalls, control_fd), answers_(&answers) {}

  /// Waits at most `wait_ms` for the pipe (0 looks and returns) and reads what
  /// is there. A failure of the pipe itself is the worker's.
  [[nodiscard]] core::Result<ControlLook, WorkerError> look(int wait_ms);

  /// The acknowledgement of message `sequence`.
  [[nodiscard]] Delivery answer(std::uint32_t sequence, AckStatus status, std::string detail);

 private:
  platform::Syscalls* syscalls_;
  int fd_;
  ipc::RecordSource source_;
  ipc::RecordWriter* answers_;
};

}  // namespace loadforge::worker

#endif
