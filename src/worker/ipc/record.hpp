// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_IPC_RECORD_HPP
#define LOADFORGE_WORKER_IPC_RECORD_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::ipc {

/// The record types a worker sends at M1. The frame carries the raw number,
/// and a reader hands back whatever number it found: an unknown type is a
/// record the consumer does not understand, which is a different fact from a
/// frame that is corrupt, and the two must not be confused.
///
/// The base type is the wire width of the type field, which is why the
/// enum-size check is answered rather than obeyed: a byte would fit today's
/// three values and would not be the type a frame carries.
// NOLINTNEXTLINE(performance-enum-size)
enum class RecordType : std::uint16_t {
  kStarted = 1,   ///< The worker is running: payload is its own description.
  kFinished = 2,  ///< The worker completed its work: payload is its summary.
  kError = 3,     ///< The worker could not continue: payload is the reason.
};

[[nodiscard]] std::string_view describe(RecordType type) noexcept;

/// One record as the reader hands it back: the type number and the bytes.
struct Record {
  std::uint16_t type = 0;
  std::vector<std::byte> payload;

  [[nodiscard]] friend bool operator==(const Record&, const Record&) = default;
};

/// Why a byte stream is not a record stream.
///
///   kBadMagic      the next bytes do not begin a frame.
///   kBadVersion    a frame of a version this build does not read.
///   kOversized     a length past kMaxPayloadBytes: not a frame this build
///                  would ever write, so not one it will read.
///   kBadChecksum   the frame arrived, and its bytes are not the ones sent.
///   kTruncated     the stream ended inside a frame: the writer died, or was
///                  killed, mid-record.
enum class FrameFault : std::uint8_t {
  kBadMagic,
  kBadVersion,
  kOversized,
  kBadChecksum,
  kTruncated,
};

[[nodiscard]] std::string_view describe(FrameFault fault) noexcept;

// The same narrow, provably safe suppression SyscallError carries, for the
// same reason: the analyzer cannot follow a value through std::variant and
// treats a FrameError copied out of a Result as unconstructed. Every field
// has a default member initialiser, so the condition cannot occur.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct FrameError {
  FrameFault fault = FrameFault::kBadMagic;
  std::string detail;  ///< What was found, and what was expected.

  [[nodiscard]] friend bool operator==(const FrameError&, const FrameError&) = default;
};

[[nodiscard]] std::string describe(const FrameError& error);

/// Wire format, little-endian throughout:
///
///   offset  size  field
///        0     4  magic     kFrameMagic
///        4     2  version   kFrameVersion
///        6     2  type
///        8     4  length    of the payload, at most kMaxPayloadBytes
///       12     4  crc32     over bytes 0..11 and the payload
///       16     n  payload
///
/// The checksum covers the header so that a corrupted length or type is a
/// checksum fault and not a plausible frame of the wrong shape.
inline constexpr std::uint32_t kFrameMagic = 0x4C465243;  // "LFRC"
inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 16;

/// The largest payload a frame carries. A worker's records are a few hundred
/// bytes; a megabyte would mean a bug, and a reader that allocated whatever a
/// corrupt length field said would be the easiest thing on the machine to
/// crash. Framed as a limit on the WRITER too, so both sides agree.
inline constexpr std::size_t kMaxPayloadBytes = std::size_t{64} << 10U;

/// One frame, ready to write. Refuses an oversized payload rather than
/// producing a frame no reader would accept.
[[nodiscard]] core::Result<std::vector<std::byte>, FrameError> encode(
    std::uint16_t type, std::span<const std::byte> payload);

/// Turns bytes, in whatever pieces they arrive, back into records.
///
/// NEVER RESYNCHRONISES
/// --------------------
/// A reader that skipped ahead looking for the next magic after a bad frame
/// would turn one corrupt byte into a silently shortened stream, and a
/// shortened stream of findings is a hardware verdict with evidence missing.
/// The first fault is final: every later call reports the same fault, and the
/// consumer decides what to do about a worker whose channel went bad.
class RecordReader {
 public:
  /// More bytes from the stream. Any amount, including none.
  void feed(std::span<const std::byte> bytes);

  /// The next complete record; nothing if the buffered bytes do not yet hold
  /// one; the fault, forever, once one has been found.
  [[nodiscard]] core::Result<std::optional<Record>, FrameError> next();

  /// The stream has ended. Bytes left inside a frame are kTruncated; none
  /// left is a clean end. Like every fault, truncation is permanent.
  [[nodiscard]] core::Result<core::Ok, FrameError> finish();

  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool faulted() const noexcept { return fault_.has_value(); }

 private:
  std::vector<std::byte> buffer_;
  std::optional<FrameError> fault_;
};

/// What a channel operation could not do: frame the bytes, or move them.
struct ChannelError {
  std::optional<FrameError> frame;  ///< Set when the fault is in the framing.
  platform::SyscallError cause;     ///< Valid when `frame` is not set.

  [[nodiscard]] friend bool operator==(const ChannelError&, const ChannelError&) = default;
};

[[nodiscard]] std::string describe(const ChannelError& error);

/// The worker's end: frames a record and writes it whole.
///
/// Does not own the descriptor. The pipe ends belong to whoever created the
/// pipe and handed one end across exec; this writes to one.
class RecordWriter {
 public:
  /// Consecutive EINTRs tolerated before a send is abandoned, for the reason
  /// FileSystem bounds its reads: an unbounded retry turns a signal storm into
  /// a hang, and a hung worker is the one outcome worse than a failed one.
  static constexpr int kMaxConsecutiveInterrupts = 128;

  RecordWriter(platform::Syscalls& syscalls, int fd) noexcept : syscalls_(&syscalls), fd_(fd) {}

  [[nodiscard]] int fd() const noexcept { return fd_; }

  /// Writes one frame, looping over short writes. A write that transfers
  /// nothing is reported as a send failure rather than retried, because a
  /// loop that could spin on it would be a hang with no evidence.
  [[nodiscard]] core::Result<core::Ok, ChannelError> send(std::uint16_t type,
                                                          std::span<const std::byte> payload);

  [[nodiscard]] core::Result<core::Ok, ChannelError> send(RecordType type,
                                                          std::span<const std::byte> payload) {
    return send(static_cast<std::uint16_t>(type), payload);
  }

 private:
  platform::Syscalls* syscalls_;
  int fd_;
};

/// One drain of a source: the records that one read completed, and whether
/// the stream has ended. Records and the end can arrive together -- a writer
/// that sent its last record and exited -- so this is a pair, not a choice.
struct Drained {
  std::vector<Record> records;
  bool end_of_stream = false;

  [[nodiscard]] friend bool operator==(const Drained&, const Drained&) = default;
};

/// The controller's end of one worker's channel: reads what poll said is
/// there and hands back whole records.
///
/// Does not own the descriptor, for the reason RecordWriter does not.
class RecordSource {
 public:
  static constexpr std::size_t kChunkBytes = 4096;
  static constexpr int kMaxConsecutiveInterrupts = RecordWriter::kMaxConsecutiveInterrupts;

  RecordSource(platform::Syscalls& syscalls, int fd) noexcept : syscalls_(&syscalls), fd_(fd) {}

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] bool at_end() const noexcept { return ended_; }

  /// One read(2), then every record it completed. Call after poll reports
  /// the descriptor readable or hung up; a read of an open, empty pipe would
  /// block. After the end of the stream this reads nothing and says so.
  [[nodiscard]] core::Result<Drained, ChannelError> drain();

 private:
  platform::Syscalls* syscalls_;
  int fd_;
  RecordReader reader_;
  bool ended_ = false;
};

}  // namespace loadforge::ipc

#endif
