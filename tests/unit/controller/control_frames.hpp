// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_CONTROL_FRAMES_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_CONTROL_FRAMES_HPP

// The controller's tests play the worker's part on the control and record
// pipes: the frames a controller writes (to be read back and compared) and the
// frames a worker answers with.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "worker/control.hpp"
#include "worker/ipc/record.hpp"

namespace loadforge::controller_testing {

inline std::string text_of_bytes(const std::vector<std::byte>& bytes) {
  std::string text;
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

/// A message as the controller puts it on a worker's control pipe.
inline std::string frame_of_message(const worker::ControlMessage& message) {
  const std::vector<std::byte> payload = worker::encode(message);
  return text_of_bytes(ipc::encode(worker::kControlFrameType, payload).value());
}

/// An acknowledgement as a worker puts it on its record pipe.
inline std::string frame_of_ack(const worker::ControlAck& ack) {
  const std::vector<std::byte> payload = worker::encode(ack);
  return text_of_bytes(ipc::encode(worker::kControlAckFrameType, payload).value());
}

/// The messages in what the controller wrote on a control pipe, in order.
inline std::vector<worker::ControlMessage> messages_in(const std::string& written) {
  std::vector<worker::ControlMessage> out;
  ipc::RecordReader reader;
  std::vector<std::byte> raw;
  for (const char c : written) {
    raw.push_back(static_cast<std::byte>(c));
  }
  reader.feed(raw);
  while (true) {
    auto next = reader.next();
    if (!next.has_value() || !next.value().has_value()) {
      break;
    }
    out.push_back(worker::decode_message(next.value()->payload).value());
  }
  return out;
}

}  // namespace loadforge::controller_testing

#endif
