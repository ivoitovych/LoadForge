// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKER_CONTROL_FRAMES_HPP
#define LOADFORGE_TESTS_UNIT_WORKER_CONTROL_FRAMES_HPP

// A control message as the controller puts it on the pipe, and what the worker
// answered, for the tests that play the controller's part.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "worker/control.hpp"
#include "worker/ipc/record.hpp"

namespace loadforge::worker_testing {

/// The bytes of a frame, as text.
inline std::string text_of(const std::vector<std::byte>& bytes) {
  std::string text;
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

/// One message as the controller frames it.
inline std::string control_frame(const worker::ControlMessage& message) {
  const std::vector<std::byte> payload = worker::encode(message);
  return text_of(ipc::encode(worker::kControlFrameType, payload).value());
}

/// The acknowledgements in what a worker wrote on its record pipe, decoded,
/// in order.
inline std::vector<worker::ControlAck> acknowledgements_in(const std::string& written) {
  std::vector<worker::ControlAck> out;
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
    if (next.value()->type == worker::kControlAckFrameType) {
      out.push_back(worker::decode_ack(next.value()->payload).value());
    }
  }
  return out;
}

}  // namespace loadforge::worker_testing

#endif
