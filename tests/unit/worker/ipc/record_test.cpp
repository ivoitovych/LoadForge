// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record channel: checksummed frames over a pipe, a reader that reports
// the first corruption and never heals around it, and the two ends that move
// the bytes.

#include "worker/ipc/record.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/crc32.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::ipc {
namespace {

using loadforge::platform::PipeEnds;
using loadforge::platform::RealSyscalls;
using loadforge::platform::SyscallError;
using loadforge::testing::FakeSyscalls;

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

/// A frame moved out of its Result; an encode failure is a test failure.
std::vector<std::byte> frame_of(std::uint16_t type, std::string_view payload) {
  auto encoded = encode(type, bytes_of(payload));
  EXPECT_TRUE(encoded.has_value()) << describe(encoded.error());
  return encoded.has_value() ? encoded.value() : std::vector<std::byte>{};
}

/// The next record, moved out of two layers of Result/optional; absence or a
/// fault is a test failure here, so the tests that expect those say so.
Record record_of(RecordReader& reader) {
  auto next = reader.next();
  EXPECT_TRUE(next.has_value()) << describe(next.error());
  EXPECT_TRUE(next.has_value() && next.value().has_value()) << "no record ready";
  return next.has_value() && next.value().has_value() ? *next.value() : Record{};
}

PipeEnds pipe_of(FakeSyscalls& syscalls) {
  auto created = syscalls.create_pipe();
  EXPECT_TRUE(created.has_value()) << describe(created.error());
  return created.has_value() ? created.value() : PipeEnds{};
}

// --- words -------------------------------------------------------------------

TEST(RecordWordsTest, EveryTypeAndFaultHasItsOwnWordsAndTheTablesAreTotal) {
  EXPECT_EQ(describe(RecordType::kStarted), "started");
  EXPECT_EQ(describe(RecordType::kFinished), "finished");
  EXPECT_EQ(describe(RecordType::kError), "error");
  EXPECT_EQ(describe(static_cast<RecordType>(200)), "unrecognised record type");

  EXPECT_EQ(describe(FrameFault::kBadMagic), "the bytes do not begin a record frame");
  EXPECT_EQ(describe(FrameFault::kBadVersion),
            "the frame is of a version this build does not read");
  EXPECT_EQ(describe(FrameFault::kOversized),
            "the frame claims a payload larger than any this build writes");
  EXPECT_EQ(describe(FrameFault::kBadChecksum), "the frame's checksum does not match its bytes");
  EXPECT_EQ(describe(FrameFault::kTruncated), "the stream ended inside a frame");
  EXPECT_EQ(describe(static_cast<FrameFault>(200)), "unrecognised frame fault");
}

TEST(RecordWordsTest, ErrorsRenderTheirDetailAndAChannelErrorPicksTheRightHalf) {
  EXPECT_EQ(describe(FrameError{FrameFault::kTruncated, ""}), "the stream ended inside a frame");
  EXPECT_EQ(describe(FrameError{FrameFault::kBadMagic, "magic 7, expected 1279676995"}),
            "the bytes do not begin a record frame: magic 7, expected 1279676995");

  const ChannelError framing{FrameError{FrameFault::kBadVersion, "version 2, expected 1"},
                             SyscallError{}};
  EXPECT_EQ(describe(framing),
            "the frame is of a version this build does not read: version 2, expected 1");
  const ChannelError moving{std::nullopt, SyscallError{EPIPE, "write", "fd 201"}};
  EXPECT_EQ(describe(moving), "write(fd 201): Broken pipe (errno 32)");
}

TEST(RecordWordsTest, EqualityIsFieldByField) {
  const Record a{1, bytes_of("x")};
  EXPECT_EQ(a, (Record{1, bytes_of("x")}));
  EXPECT_NE(a, (Record{2, bytes_of("x")}));
  EXPECT_NE(a, (Record{1, bytes_of("y")}));

  const FrameError e{FrameFault::kBadMagic, "d"};
  EXPECT_EQ(e, (FrameError{FrameFault::kBadMagic, "d"}));
  EXPECT_NE(e, (FrameError{FrameFault::kBadVersion, "d"}));
  EXPECT_NE(e, (FrameError{FrameFault::kBadMagic, "e"}));

  const ChannelError c{std::nullopt, SyscallError{1, "a", "b"}};
  EXPECT_EQ(c, (ChannelError{std::nullopt, SyscallError{1, "a", "b"}}));
  EXPECT_NE(c, (ChannelError{FrameError{}, SyscallError{1, "a", "b"}}));
  EXPECT_NE(c, (ChannelError{std::nullopt, SyscallError{2, "a", "b"}}));

  const Drained d{{a}, false};
  EXPECT_EQ(d, (Drained{{a}, false}));
  EXPECT_NE(d, (Drained{{a}, true}));
  EXPECT_NE(d, (Drained{{}, false}));
}

// --- framing -----------------------------------------------------------------

TEST(FrameTest, AFrameIsTheDocumentedBytesInLittleEndianOrder) {
  const std::vector<std::byte> frame = frame_of(0x0102, "hi");
  ASSERT_EQ(frame.size(), kFrameHeaderBytes + 2);
  const std::vector<std::byte> head{
      std::byte{0x43}, std::byte{0x52}, std::byte{0x46}, std::byte{0x4C},  // "LFRC" as u32 LE
      std::byte{0x01}, std::byte{0x00},                                    // version 1
      std::byte{0x02}, std::byte{0x01},                                    // type 0x0102
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // length 2
  };
  EXPECT_TRUE(std::equal(head.begin(), head.end(), frame.begin()));
  // The checksum covers the twelve header bytes then the payload.
  std::vector<std::byte> covered(frame.begin(), frame.begin() + 12);
  covered.push_back(std::byte{'h'});
  covered.push_back(std::byte{'i'});
  const std::uint32_t expected = core::crc32(covered);
  std::uint32_t stored = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    stored |= std::to_integer<std::uint32_t>(frame[12 + index]) << (8U * index);
  }
  EXPECT_EQ(stored, expected);
  EXPECT_EQ(frame[16], std::byte{'h'});
  EXPECT_EQ(frame[17], std::byte{'i'});
}

TEST(FrameTest, AnEmptyPayloadAndTheLargestPayloadBothFrameAndOneMoreByteDoesNot) {
  EXPECT_EQ(frame_of(1, "").size(), kFrameHeaderBytes);
  const std::vector<std::byte> largest(kMaxPayloadBytes, std::byte{0x5a});
  auto at_limit = encode(1, largest);
  ASSERT_TRUE(at_limit.has_value());
  EXPECT_EQ(at_limit.value().size(), kFrameHeaderBytes + kMaxPayloadBytes);

  const std::vector<std::byte> too_large(kMaxPayloadBytes + 1, std::byte{0x5a});
  auto refused = encode(1, too_large);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, FrameFault::kOversized);
  EXPECT_EQ(refused.error().detail, "payload bytes 65537, expected 65536");
}

TEST(RecordReaderTest, ReadsBackWhatWasEncodedAndTheTypeNumberIsCarriedAsIs) {
  RecordReader reader;
  EXPECT_EQ(reader.buffered(), 0U);
  reader.feed(frame_of(static_cast<std::uint16_t>(RecordType::kStarted), "worker 3"));
  reader.feed(frame_of(900, "a type this build does not know"));
  const Record first = record_of(reader);
  EXPECT_EQ(first.type, 1);
  EXPECT_EQ(text_of(first.payload), "worker 3");
  const Record second = record_of(reader);
  EXPECT_EQ(second.type, 900) << "an unknown type is the consumer's problem, not a frame fault";
  EXPECT_EQ(text_of(second.payload), "a type this build does not know");
  EXPECT_EQ(reader.buffered(), 0U);
  auto finished = reader.finish();
  EXPECT_TRUE(finished.has_value());
  EXPECT_FALSE(reader.faulted());
}

TEST(RecordReaderTest, NothingIsReadyUntilTheWholeFrameHasArrivedHoweverItIsSplit) {
  const std::vector<std::byte> frame = frame_of(2, "payload of some length");
  // Every prefix of the frame yields nothing; the last byte completes it.
  for (std::size_t prefix = 0; prefix < frame.size(); ++prefix) {
    RecordReader reader;
    reader.feed(std::span<const std::byte>{frame}.first(prefix));
    auto next = reader.next();
    ASSERT_TRUE(next.has_value()) << "prefix " << prefix << ": " << describe(next.error());
    EXPECT_FALSE(next.value().has_value()) << "prefix " << prefix;
    EXPECT_EQ(reader.buffered(), prefix);
    reader.feed(std::span<const std::byte>{frame}.subspan(prefix));
    EXPECT_EQ(text_of(record_of(reader).payload), "payload of some length") << "prefix " << prefix;
  }
  // Byte at a time, two frames back to back.
  RecordReader reader;
  std::vector<std::byte> stream = frame_of(1, "one");
  const std::vector<std::byte> second = frame_of(2, "two");
  stream.insert(stream.end(), second.begin(), second.end());
  std::vector<std::string> seen;
  for (const std::byte byte : stream) {
    reader.feed(std::span<const std::byte>{&byte, 1});
    auto next = reader.next();
    ASSERT_TRUE(next.has_value());
    if (next.value().has_value()) {
      seen.push_back(text_of(next.value()->payload));
    }
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"one", "two"}));
}

TEST(RecordReaderTest, EachHeaderCorruptionHasItsOwnFaultNamingFoundAndExpected) {
  const std::vector<std::byte> good = frame_of(1, "abc");

  std::vector<std::byte> bad_magic = good;
  bad_magic[0] = std::byte{0x00};
  RecordReader reader;
  reader.feed(bad_magic);
  auto next = reader.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kBadMagic);
  EXPECT_EQ(next.error().detail, "magic 1279676928, expected 1279676995");

  std::vector<std::byte> bad_version = good;
  bad_version[4] = std::byte{0x02};
  RecordReader versioned;
  versioned.feed(bad_version);
  next = versioned.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kBadVersion);
  EXPECT_EQ(next.error().detail, "version 2, expected 1");

  std::vector<std::byte> oversized = good;
  oversized[10] = std::byte{0x01};  // length 3 + (1 << 16) = 65539
  RecordReader sized;
  sized.feed(oversized);
  next = sized.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kOversized);
  EXPECT_EQ(next.error().detail, "payload bytes 65539, expected 65536");
  EXPECT_EQ(sized.buffered(), good.size()) << "an oversized frame is never waited for";

  std::vector<std::byte> bad_payload = good;
  bad_payload[kFrameHeaderBytes + 1] = std::byte{'X'};
  RecordReader summed;
  summed.feed(bad_payload);
  next = summed.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kBadChecksum);
  std::vector<std::byte> covered(bad_payload.begin(), bad_payload.begin() + 12);
  covered.insert(covered.end(), bad_payload.begin() + 16, bad_payload.end());
  std::uint32_t stored = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    stored |= std::to_integer<std::uint32_t>(good[12 + index]) << (8U * index);
  }
  EXPECT_EQ(next.error().detail, "checksum " + std::to_string(stored) + ", expected " +
                                     std::to_string(core::crc32(covered)));

  // A corrupted TYPE is a checksum fault, not a record of another type: the
  // checksum covers the header.
  std::vector<std::byte> bad_type = good;
  bad_type[6] = std::byte{0x03};
  RecordReader typed;
  typed.feed(bad_type);
  next = typed.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kBadChecksum);
}

TEST(RecordReaderTest, AFaultIsFinalAndTheReaderNeverResynchronises) {
  // A bad frame followed by a perfectly good one: the good one is never
  // delivered, because a reader that skipped to it would have turned one
  // corrupt byte into a quietly shorter stream of findings.
  std::vector<std::byte> stream = frame_of(1, "corrupted");
  stream[0] = std::byte{0xFF};
  const std::vector<std::byte> good = frame_of(2, "good");
  stream.insert(stream.end(), good.begin(), good.end());
  RecordReader reader;
  reader.feed(stream);
  auto first = reader.next();
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().fault, FrameFault::kBadMagic);
  EXPECT_TRUE(reader.faulted());
  auto again = reader.next();
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error(), first.error());
  reader.feed(good);
  auto after_more = reader.next();
  ASSERT_FALSE(after_more.has_value());
  EXPECT_EQ(after_more.error(), first.error());
  auto finished = reader.finish();
  ASSERT_FALSE(finished.has_value());
  EXPECT_EQ(finished.error(), first.error()) << "finish reports the fault already found";
}

TEST(RecordReaderTest, AStreamThatEndsInsideAFrameIsTruncatedAndStaysSo) {
  const std::vector<std::byte> frame = frame_of(1, "cut short");
  RecordReader reader;
  reader.feed(std::span<const std::byte>{frame}.first(frame.size() - 1));
  auto next = reader.next();
  ASSERT_TRUE(next.has_value());
  EXPECT_FALSE(next.value().has_value());
  auto finished = reader.finish();
  ASSERT_FALSE(finished.has_value());
  EXPECT_EQ(finished.error().fault, FrameFault::kTruncated);
  EXPECT_EQ(finished.error().detail, "bytes left 24, expected 0");
  EXPECT_TRUE(reader.faulted());
  // Permanent: more bytes, even the missing one, do not heal it.
  reader.feed(std::span<const std::byte>{frame}.last(1));
  next = reader.next();
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().fault, FrameFault::kTruncated);
  // And a header alone, short of the frame it announces, is truncated too.
  RecordReader header_only;
  header_only.feed(std::span<const std::byte>{frame}.first(kFrameHeaderBytes));
  EXPECT_EQ(header_only.finish().error().fault, FrameFault::kTruncated);
  // A clean end is a clean end, twice.
  RecordReader clean;
  EXPECT_TRUE(clean.finish().has_value());
  EXPECT_TRUE(clean.finish().has_value());
}

TEST(RecordReaderTest, EverySingleByteCorruptionOfAValidStreamIsCaughtAndNeverYieldsAWrongRecord) {
  // The property the channel exists for. A stream of frames, every byte of it
  // corrupted in turn (every position, a flipped bit and a random
  // replacement): the reader must report a fault at or before the corrupted
  // frame, must deliver every frame before it intact, and must never deliver
  // a record that is not one of the originals. The chunking is random too,
  // because the reader's answer must not depend on how the bytes arrived.
  std::mt19937 random(20261018);
  std::vector<std::vector<std::byte>> frames;
  std::vector<std::byte> stream;
  for (std::uint16_t index = 0; index < 5; ++index) {
    const std::string payload(static_cast<std::size_t>(index) * 7, static_cast<char>('a' + index));
    frames.push_back(frame_of(index, payload));
    stream.insert(stream.end(), frames.back().begin(), frames.back().end());
  }
  std::vector<Record> originals;
  for (const auto& frame : frames) {
    RecordReader one;
    one.feed(frame);
    originals.push_back(record_of(one));
  }

  std::uniform_int_distribution<int> bit(0, 7);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<std::size_t> chunk(1, 9);
  std::size_t faults = 0;
  for (std::size_t position = 0; position < stream.size(); ++position) {
    for (int variant = 0; variant < 2; ++variant) {
      std::vector<std::byte> corrupted = stream;
      const std::byte replacement =
          variant == 0 ? corrupted[position] ^ static_cast<std::byte>(1U << bit(random))
                       : static_cast<std::byte>(byte(random));
      if (replacement == corrupted[position]) {
        continue;  // The random replacement happened to be the original.
      }
      corrupted[position] = replacement;

      // Which frame holds the corrupted byte: everything before it must
      // still be delivered intact.
      std::size_t frame_index = 0;
      for (std::size_t offset = frames[0].size(); offset <= position;
           offset += frames[frame_index].size()) {
        ++frame_index;
      }

      RecordReader reader;
      std::size_t delivered = 0;
      std::optional<FrameError> fault;
      std::size_t fed = 0;
      while (fed < corrupted.size() && !fault.has_value()) {
        const std::size_t piece = std::min(chunk(random), corrupted.size() - fed);
        reader.feed(std::span<const std::byte>{corrupted}.subspan(fed, piece));
        fed += piece;
        while (!fault.has_value()) {
          auto next = reader.next();
          if (!next.has_value()) {
            fault = next.error();
          } else if (!next.value().has_value()) {
            break;
          } else {
            ASSERT_LT(delivered, frame_index) << "position " << position;
            EXPECT_EQ(*next.value(), originals[delivered]) << "position " << position;
            ++delivered;
          }
        }
      }
      if (!fault.has_value()) {
        auto finished = reader.finish();
        ASSERT_FALSE(finished.has_value()) << "position " << position << " variant " << variant
                                           << ": the corruption went unnoticed";
        fault = finished.error();
      }
      EXPECT_EQ(delivered, frame_index) << "position " << position;
      ++faults;
    }
  }
  EXPECT_GT(faults, stream.size());
}

TEST(RecordReaderTest, RandomBytesNeverCrashTheReaderOrYieldARecord) {
  std::mt19937 random(42);
  std::uniform_int_distribution<int> byte(0, 255);
  for (int round = 0; round < 200; ++round) {
    std::vector<std::byte> noise(static_cast<std::size_t>(round) * 3);
    for (auto& b : noise) {
      b = static_cast<std::byte>(byte(random));
    }
    RecordReader reader;
    reader.feed(noise);
    auto next = reader.next();
    if (next.has_value()) {
      EXPECT_FALSE(next.value().has_value()) << "round " << round;
      EXPECT_LT(noise.size(), kFrameHeaderBytes) << "a full header of noise must fault";
    }
  }
}

// --- the writer ----------------------------------------------------------------

TEST(RecordWriterTest, WritesTheWholeFrameAcrossShortWrites) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  EXPECT_EQ(writer.fd(), pipe.write_fd);
  syscalls.script_writes({{5, 0}, {1, 0}, {3, 0}});  // Then unscripted: the rest.
  auto sent = writer.send(RecordType::kFinished, bytes_of("done"));
  ASSERT_TRUE(sent.has_value()) << describe(sent.error());
  EXPECT_EQ(syscalls.write_count(), 4);
  RecordReader reader;
  reader.feed(bytes_of(syscalls.pipe_contents(pipe.write_fd)));
  const Record record = record_of(reader);
  EXPECT_EQ(record.type, static_cast<std::uint16_t>(RecordType::kFinished));
  EXPECT_EQ(text_of(record.payload), "done");
  EXPECT_TRUE(reader.finish().has_value());
}

TEST(RecordWriterTest, RetriesInterruptionsUpToTheBoundAndReportsTheBound) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  syscalls.script_writes({{0, EINTR}, {4, 0}, {0, EINTR}, {0, EINTR}});
  auto sent = writer.send(1, bytes_of("x"));
  ASSERT_TRUE(sent.has_value()) << describe(sent.error());
  EXPECT_EQ(syscalls.write_count(), 5) << "three interruptions, two transfers";

  FakeSyscalls stormed;
  const PipeEnds other = pipe_of(stormed);
  RecordWriter victim(stormed, other.write_fd);
  stormed.always_fail_write(EINTR);
  auto abandoned = victim.send(1, bytes_of("x"));
  ASSERT_FALSE(abandoned.has_value());
  ASSERT_FALSE(abandoned.error().frame.has_value());
  EXPECT_EQ(abandoned.error().cause.number, EINTR);
  EXPECT_EQ(stormed.write_count(), RecordWriter::kMaxConsecutiveInterrupts);
}

TEST(RecordWriterTest, AWriteThatFailsIsReportedAsItsErrno) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  ASSERT_TRUE(syscalls.close(pipe.read_fd).has_value());
  auto sent = writer.send(1, bytes_of("nobody listening"));
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error(), (ChannelError{std::nullopt, SyscallError{EPIPE, "write", "fd 201"}}));
  EXPECT_EQ(describe(sent.error()), "write(fd 201): Broken pipe (errno 32)");

  RecordWriter wrong_end(syscalls, pipe.read_fd);
  auto refused = wrong_end.send(1, bytes_of("x"));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().cause.number, EBADF);
}

TEST(RecordWriterTest, AWriteThatTransfersNothingIsASendFailureNotASpin) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  syscalls.script_writes({{3, 0}, {0, 0}});
  auto sent = writer.send(1, bytes_of("x"));
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error(),
            (ChannelError{std::nullopt,
                          SyscallError{EIO, "send", "fd 201 (write transferred nothing)"}}));
  EXPECT_EQ(syscalls.write_count(), 2);
}

TEST(RecordWriterTest, AnOversizedPayloadIsRefusedBeforeAnythingIsWritten) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  const std::vector<std::byte> too_large(kMaxPayloadBytes + 1, std::byte{0});
  auto sent = writer.send(1, too_large);
  ASSERT_FALSE(sent.has_value());
  ASSERT_TRUE(sent.error().frame.has_value());
  EXPECT_EQ(sent.error().frame->fault, FrameFault::kOversized);
  EXPECT_EQ(syscalls.write_count(), 0);
}

// --- the source ----------------------------------------------------------------

TEST(RecordSourceTest, DrainsWhatOneReadCompletesAndKeepsTheRestForTheNext) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  RecordSource source(syscalls, pipe.read_fd);
  EXPECT_EQ(source.fd(), pipe.read_fd);
  ASSERT_TRUE(writer.send(1, bytes_of("first")).has_value());
  ASSERT_TRUE(writer.send(2, bytes_of("second")).has_value());
  ASSERT_TRUE(writer.send(3, bytes_of("third")).has_value());
  // Deliver in pieces that straddle frames: 20 bytes is one frame of "first"
  // (21 bytes) short by one.
  syscalls.limit_pipe_reads(20);
  auto first = source.drain();
  ASSERT_TRUE(first.has_value()) << describe(first.error());
  EXPECT_TRUE(first.value().records.empty());
  EXPECT_FALSE(first.value().end_of_stream);
  syscalls.limit_pipe_reads(30);
  auto second = source.drain();
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second.value().records.size(), 2U) << "the first completed, the second arrived whole";
  EXPECT_EQ(text_of(second.value().records[0].payload), "first");
  EXPECT_EQ(text_of(second.value().records[1].payload), "second");
  syscalls.limit_pipe_reads(4096);
  auto third = source.drain();
  ASSERT_TRUE(third.has_value());
  ASSERT_EQ(third.value().records.size(), 1U);
  EXPECT_EQ(third.value().records[0].type, 3);
  EXPECT_FALSE(third.value().end_of_stream);
  EXPECT_FALSE(source.at_end());
}

TEST(RecordSourceTest, TheWriterClosingIsACleanEndAtAFrameBoundary) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordWriter writer(syscalls, pipe.write_fd);
  RecordSource source(syscalls, pipe.read_fd);
  ASSERT_TRUE(writer.send(1, bytes_of("last")).has_value());
  ASSERT_TRUE(syscalls.close(pipe.write_fd).has_value());
  auto drained = source.drain();
  ASSERT_TRUE(drained.has_value());
  ASSERT_EQ(drained.value().records.size(), 1U);
  EXPECT_FALSE(drained.value().end_of_stream) << "the end is seen by the read that returns 0";
  auto ended = source.drain();
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended.value(), (Drained{{}, true}));
  EXPECT_TRUE(source.at_end());
  const int reads_before = syscalls.read_count();
  auto after = source.drain();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after.value(), (Drained{{}, true}));
  EXPECT_EQ(syscalls.read_count(), reads_before) << "nothing is read past the end";
}

TEST(RecordSourceTest, TheWriterDyingMidFrameIsTruncationAndTheEndIsStillReached) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordSource source(syscalls, pipe.read_fd);
  const std::vector<std::byte> frame = frame_of(1, "never finished");
  syscalls.deliver_to_pipe(pipe.read_fd,
                           text_of(std::vector<std::byte>(frame.begin(), frame.begin() + 10)));
  ASSERT_TRUE(syscalls.close(pipe.write_fd).has_value());
  auto partial = source.drain();
  ASSERT_TRUE(partial.has_value());
  EXPECT_TRUE(partial.value().records.empty());
  auto ended = source.drain();
  ASSERT_FALSE(ended.has_value());
  ASSERT_TRUE(ended.error().frame.has_value());
  EXPECT_EQ(ended.error().frame->fault, FrameFault::kTruncated);
  EXPECT_EQ(ended.error().frame->detail, "bytes left 10, expected 0");
  EXPECT_TRUE(source.at_end());
}

TEST(RecordSourceTest, ACorruptFrameIsReportedWithTheRecordsBeforeItLost) {
  // The records drained in the same read as the corruption are not handed
  // back: a drain either succeeds whole or reports the fault. The caller's
  // answer to a corrupt channel does not depend on partial findings.
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordSource source(syscalls, pipe.read_fd);
  std::vector<std::byte> stream = frame_of(1, "fine");
  std::vector<std::byte> bad = frame_of(2, "corrupt");
  bad[5] = std::byte{0x09};  // version 0x0901
  stream.insert(stream.end(), bad.begin(), bad.end());
  syscalls.deliver_to_pipe(pipe.read_fd, text_of(stream));
  auto drained = source.drain();
  ASSERT_FALSE(drained.has_value());
  ASSERT_TRUE(drained.error().frame.has_value());
  EXPECT_EQ(drained.error().frame->fault, FrameFault::kBadVersion);
  EXPECT_EQ(drained.error().frame->detail, "version 2305, expected 1");
  const int reads_before = syscalls.read_count();
  auto again = source.drain();
  ASSERT_FALSE(again.has_value()) << "the fault is final";
  EXPECT_EQ(again.error(), drained.error());
  EXPECT_EQ(syscalls.read_count(), reads_before) << "nothing is read past a fault";
  EXPECT_FALSE(source.at_end()) << "faulted is not ended; the caller closes the channel";
}

TEST(RecordSourceTest, ReadFailuresAreReportedAndInterruptionsRetriedUpToTheBound) {
  FakeSyscalls syscalls;
  const PipeEnds pipe = pipe_of(syscalls);
  RecordSource source(syscalls, pipe.read_fd);
  syscalls.deliver_to_pipe(pipe.read_fd, text_of(frame_of(1, "after the storm")));
  syscalls.script_reads({{"", EINTR}, {"", EINTR}});
  auto drained = source.drain();
  ASSERT_TRUE(drained.has_value()) << describe(drained.error());
  ASSERT_EQ(drained.value().records.size(), 1U);
  EXPECT_EQ(syscalls.read_count(), 3);

  auto empty = source.drain();
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().cause.number, EAGAIN) << "the fake refuses to block";

  syscalls.always_fail_read(EINTR);
  const int before = syscalls.read_count();
  auto abandoned = source.drain();
  ASSERT_FALSE(abandoned.has_value());
  ASSERT_FALSE(abandoned.error().frame.has_value());
  EXPECT_EQ(abandoned.error().cause.number, EINTR);
  EXPECT_EQ(syscalls.read_count() - before, RecordSource::kMaxConsecutiveInterrupts);
  EXPECT_FALSE(source.at_end());

  RecordSource wrong_end(syscalls, pipe.write_fd);
  syscalls.always_fail_read(0);
  auto refused = wrong_end.drain();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().cause.number, EBADF);
}

// --- the pipe, really ------------------------------------------------------------

TEST(RealProcessRecordChannel, AChildWritesRecordsThroughARealPipeAndTheParentReadsThemToTheEnd) {
  // The round trip the controller/worker split rests on, against the kernel:
  // the child holds the write end, sends three records and exits; the parent
  // closes its copy of the write end, so the child's exit is the end of the
  // stream, then polls and drains until the end arrives.
  RealSyscalls syscalls;
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  const PipeEnds pipe = created.value();

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    RealSyscalls own;
    (void)own.close(pipe.read_fd);
    RecordWriter writer(own, pipe.write_fd);
    const std::vector<std::byte> big(kMaxPayloadBytes, std::byte{0x42});
    if (!writer.send(RecordType::kStarted, bytes_of("pid")).has_value() ||
        !writer.send(RecordType::kFinished, big).has_value() ||
        !writer.send(RecordType::kError, bytes_of("none")).has_value()) {
      ::_exit(2);
    }
    ::_exit(0);
  }
  ASSERT_TRUE(syscalls.close(pipe.write_fd).has_value());

  RecordSource source(syscalls, pipe.read_fd);
  std::vector<Record> records;
  int polls = 0;
  while (!source.at_end()) {
    ASSERT_LT(polls++, 10000) << "the end never came";
    auto ready = syscalls.poll_readable({pipe.read_fd}, 5000);
    ASSERT_TRUE(ready.has_value()) << describe(ready.error());
    ASSERT_EQ(ready.value().size(), 1U);
    if (!ready.value()[0].readable && !ready.value()[0].hung_up) {
      continue;
    }
    auto drained = source.drain();
    ASSERT_TRUE(drained.has_value()) << describe(drained.error());
    records.insert(records.end(), drained.value().records.begin(), drained.value().records.end());
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "2: a send failed";
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].type, 1);
  EXPECT_EQ(text_of(records[0].payload), "pid");
  EXPECT_EQ(records[1].type, 2);
  EXPECT_EQ(records[1].payload.size(), kMaxPayloadBytes) << "a frame larger than the pipe buffer";
  EXPECT_EQ(records[2].type, 3);
  EXPECT_EQ(text_of(records[2].payload), "none");
  EXPECT_TRUE(syscalls.close(pipe.read_fd).has_value());
}

}  // namespace
}  // namespace loadforge::ipc
