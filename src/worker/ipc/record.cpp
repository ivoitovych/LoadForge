// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/ipc/record.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/crc32.hpp"
#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::ipc {

std::string_view describe(RecordType type) noexcept {
  switch (type) {
    case RecordType::kStarted:
      return "started";
    case RecordType::kFinished:
      return "finished";
    case RecordType::kError:
      return "error";
  }
  return "unrecognised record type";
}

std::string_view describe(FrameFault fault) noexcept {
  switch (fault) {
    case FrameFault::kBadMagic:
      return "the bytes do not begin a record frame";
    case FrameFault::kBadVersion:
      return "the frame is of a version this build does not read";
    case FrameFault::kOversized:
      return "the frame claims a payload larger than any this build writes";
    case FrameFault::kBadChecksum:
      return "the frame's checksum does not match its bytes";
    case FrameFault::kTruncated:
      return "the stream ended inside a frame";
  }
  return "unrecognised frame fault";
}

std::string describe(const FrameError& error) {
  std::string text{describe(error.fault)};
  if (!error.detail.empty()) {
    text += ": ";
    text += error.detail;
  }
  return text;
}

std::string describe(const ChannelError& error) {
  if (error.frame.has_value()) {
    return describe(*error.frame);
  }
  return describe(error.cause);
}

namespace {

constexpr std::size_t kChecksumOffset = 12;

// Little-endian put and get, byte by byte: the two ends are the same binary
// on the same machine, but a format written down in the header is one a
// reader can be checked against, and byte order is part of what is written.
void put_u16(std::span<std::byte> out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFU);
  out[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

// Four stores written out, as put_u16 writes its two. A loop to four would
// carry a bound no test can check: one store too many lands on the first byte
// of the next field, which is written afterwards anyway, or one past the end
// of a frame with no payload.
void put_u32(std::span<std::byte> out, std::uint32_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFU);
  out[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  out[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  out[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

std::uint16_t get_u16(std::span<const std::byte> in) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(in[0]) |
                                    (std::to_integer<unsigned>(in[1]) << 8U));
}

std::uint32_t get_u32(std::span<const std::byte> in) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= std::to_integer<std::uint32_t>(in[index]) << (8U * index);
  }
  return value;
}

std::string found_expected(std::string_view what, std::uint64_t found, std::uint64_t expected) {
  std::string text{what};
  text += " ";
  text += std::to_string(found);
  text += ", expected ";
  text += std::to_string(expected);
  return text;
}

FrameError frame_error(FrameFault fault, std::string detail) {
  return FrameError{fault, std::move(detail)};
}

ChannelError channel_error(FrameError frame) {
  return ChannelError{std::optional<FrameError>{std::move(frame)}, platform::SyscallError{}};
}

ChannelError channel_error(platform::SyscallError cause) {
  return ChannelError{std::optional<FrameError>{}, std::move(cause)};
}

/// The checksum a frame should carry: the header without its checksum field,
/// then the payload, continued rather than concatenated.
std::uint32_t checksum_of(std::span<const std::byte> header, std::span<const std::byte> payload) {
  return core::crc32_update(core::crc32(header.first(kChecksumOffset)), payload);
}

}  // namespace

core::Result<std::vector<std::byte>, FrameError> encode(std::uint16_t type,
                                                        std::span<const std::byte> payload) {
  if (payload.size() > kMaxPayloadBytes) {
    return frame_error(FrameFault::kOversized,
                       found_expected("payload bytes", payload.size(), kMaxPayloadBytes));
  }
  std::vector<std::byte> frame(kFrameHeaderBytes + payload.size());
  const std::span<std::byte> header{frame.data(), kFrameHeaderBytes};
  put_u32(header.subspan(0, 4), kFrameMagic);
  put_u16(header.subspan(4, 2), kFrameVersion);
  put_u16(header.subspan(6, 2), type);
  put_u32(header.subspan(8, 4), static_cast<std::uint32_t>(payload.size()));
  put_u32(header.subspan(kChecksumOffset, 4), checksum_of(header, payload));
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame[kFrameHeaderBytes + index] = payload[index];
  }
  return frame;
}

void RecordReader::feed(std::span<const std::byte> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

core::Result<std::optional<Record>, FrameError> RecordReader::next() {
  if (fault_.has_value()) {
    return *fault_;
  }
  if (buffer_.size() < kFrameHeaderBytes) {
    return std::optional<Record>{};
  }
  const std::span<const std::byte> header{buffer_.data(), kFrameHeaderBytes};
  // The header fields are checked in the order a reader can check them: magic
  // and version before the length is believed, the length before anything is
  // allocated or waited for, and the checksum once every byte it covers has
  // arrived. A fault found at any step is final.
  if (const std::uint32_t magic = get_u32(header.subspan(0, 4)); magic != kFrameMagic) {
    fault_ = frame_error(FrameFault::kBadMagic, found_expected("magic", magic, kFrameMagic));
    return *fault_;
  }
  if (const std::uint16_t version = get_u16(header.subspan(4, 2)); version != kFrameVersion) {
    fault_ =
        frame_error(FrameFault::kBadVersion, found_expected("version", version, kFrameVersion));
    return *fault_;
  }
  const std::uint32_t length = get_u32(header.subspan(8, 4));
  if (length > kMaxPayloadBytes) {
    fault_ = frame_error(FrameFault::kOversized,
                         found_expected("payload bytes", length, kMaxPayloadBytes));
    return *fault_;
  }
  const std::size_t frame_bytes = kFrameHeaderBytes + length;
  if (buffer_.size() < frame_bytes) {
    return std::optional<Record>{};
  }
  const std::span<const std::byte> payload =
      std::span<const std::byte>{buffer_}.subspan(kFrameHeaderBytes, length);
  const std::uint32_t stored = get_u32(header.subspan(kChecksumOffset, 4));
  const std::uint32_t computed = checksum_of(header, payload);
  if (stored != computed) {
    fault_ = frame_error(FrameFault::kBadChecksum, found_expected("checksum", stored, computed));
    return *fault_;
  }
  Record record;
  record.type = get_u16(header.subspan(6, 2));
  record.payload.assign(payload.begin(), payload.end());
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
  return std::optional<Record>{std::move(record)};
}

core::Result<core::Ok, FrameError> RecordReader::finish() {
  if (fault_.has_value()) {
    return *fault_;
  }
  if (!buffer_.empty()) {
    fault_ = frame_error(FrameFault::kTruncated, found_expected("bytes left", buffer_.size(), 0));
    return *fault_;
  }
  return core::Ok{};
}

core::Result<core::Ok, ChannelError> RecordWriter::send(std::uint16_t type,
                                                        std::span<const std::byte> payload) {
  auto encoded = encode(type, payload);
  if (!encoded.has_value()) {
    return channel_error(encoded.error());
  }
  const std::vector<std::byte>& frame = encoded.value();
  const std::span<const std::byte> bytes{frame};
  std::size_t written = 0;
  int interrupts = 0;
  while (written < bytes.size()) {
    auto count = syscalls_->write(fd_, bytes.subspan(written));
    if (!count.has_value()) {
      if (count.error().number == EINTR && ++interrupts < kMaxConsecutiveInterrupts) {
        continue;
      }
      return channel_error(count.error());
    }
    interrupts = 0;
    if (count.value() == 0) {
      // write(2) does not return 0 for a non-empty buffer on a pipe, but the
      // loop above would spin forever if it ever did. `call` names send, not
      // write, because no system call failed: this is the writer's judgement.
      std::string subject = "fd " + std::to_string(fd_) + " (write transferred nothing)";
      return channel_error(platform::SyscallError{EIO, "send", std::move(subject)});
    }
    written += count.value();
  }
  return core::Ok{};
}

core::Result<Drained, ChannelError> RecordSource::drain() {
  Drained drained;
  if (ended_) {
    drained.end_of_stream = true;
    return drained;
  }
  if (reader_.faulted()) {
    // The fault already found is the answer, and nothing more is read: bytes
    // after a corrupt frame are not a stream this reader will interpret.
    auto fault = reader_.finish();
    return channel_error(fault.error());
  }
  std::array<char, kChunkBytes> chunk{};
  int interrupts = 0;
  std::size_t count = 0;
  while (true) {
    auto result = syscalls_->read(fd_, chunk.data(), chunk.size());
    if (result.has_value()) {
      count = result.value();
      break;
    }
    if (result.error().number == EINTR && ++interrupts < kMaxConsecutiveInterrupts) {
      continue;
    }
    return channel_error(result.error());
  }
  if (count == 0) {
    ended_ = true;
    auto finished = reader_.finish();
    if (!finished.has_value()) {
      return channel_error(finished.error());
    }
    drained.end_of_stream = true;
    return drained;
  }
  reader_.feed(std::as_bytes(std::span<const char>{chunk.data(), count}));
  while (true) {
    auto next = reader_.next();
    if (!next.has_value()) {
      return channel_error(next.error());
    }
    if (!next.value().has_value()) {
      return drained;
    }
    drained.records.push_back(*next.value());
  }
}

}  // namespace loadforge::ipc
