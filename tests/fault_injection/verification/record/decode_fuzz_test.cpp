// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the record codec: corruption injected into a stream of encoded
// records, every byte of it in turn, as worker/ipc does for its frames.
//
// What may happen to a corrupted record is fixed by codec.hpp: decode()
// answers with an error, or with the record those exact bytes encode -- a
// valid record whose encoding IS the corrupted bytes. What may not happen:
// a crash, a record is_valid() refuses, or a record that re-encodes to other
// bytes, which would mean the decoder guessed. The frame's checksum is what
// keeps a different valid record off the channel in practice (record.hpp);
// this tier proves the decoder's half.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "contracts/verification/record_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/record.hpp"
#include "verification/record/codec.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

struct Stream {
  Bytes bytes;
  std::vector<std::size_t> starts;  // each record's first byte, then the end
  std::vector<ErrorRecord> records;
};

Stream stream_of_samples() {
  Stream stream;
  std::vector<ErrorRecord> records = contracts::record_samples::valid_samples();
  // Two of this tier's own: a derived difference across lines, and a bounded
  // failure with every optional field set but no provenance.
  ExactBitCorruption across = contracts::record_samples::exact(24);
  across.difference->domain = Domain::kDerived;
  across.difference->popcount = 40;
  across.difference->last_offset = 0x2000;
  records.emplace_back(across);
  NumericalVerificationFailure bounded = contracts::record_samples::numerical();
  bounded.provenance = Provenance{};
  records.emplace_back(bounded);
  for (const ErrorRecord& record : records) {
    stream.starts.push_back(stream.bytes.size());
    const Bytes bytes = encode(record);
    stream.bytes.insert(stream.bytes.end(), bytes.begin(), bytes.end());
  }
  stream.starts.push_back(stream.bytes.size());
  stream.records = std::move(records);
  return stream;
}

std::span<const std::byte> record_at(const Bytes& bytes, const Stream& stream, std::size_t index) {
  return std::span<const std::byte>(bytes).subspan(stream.starts[index],
                                                   stream.starts[index + 1] - stream.starts[index]);
}

// One corrupted stream decoded record by record, as the frames would deliver
// it. Returns the fault of the corrupted record, or nothing if it decoded.
std::optional<DecodeFault> check_corrupted_stream(const Stream& stream, const Bytes& corrupted,
                                                  std::size_t position) {
  std::optional<DecodeFault> fault;
  for (std::size_t index = 0; index + 1 < stream.starts.size(); ++index) {
    const auto decoded = decode(record_at(corrupted, stream, index));
    const bool holds_corruption =
        position >= stream.starts[index] && position < stream.starts[index + 1];
    if (!holds_corruption) {
      EXPECT_TRUE(decoded.has_value()) << "record " << index << " untouched by byte " << position;
      if (decoded.has_value()) {
        EXPECT_TRUE(contracts::record_samples::same(decoded.value(), stream.records[index]));
      }
      continue;
    }
    if (!decoded.has_value()) {
      EXPECT_FALSE(decoded.error().detail.empty()) << "byte " << position;
      fault = decoded.error().fault;
      continue;
    }
    // Decoded: it must be valid, and its own encoding must be exactly the
    // corrupted bytes -- read, not guessed.
    EXPECT_TRUE(is_valid(decoded.value())) << "byte " << position;
    const Bytes again = encode(decoded.value());
    const auto slice = record_at(corrupted, stream, index);
    EXPECT_TRUE(std::equal(again.begin(), again.end(), slice.begin(), slice.end()))
        << "byte " << position << " decoded to a record whose bytes differ: a guess";
  }
  return fault;
}

TEST(RecordDecodeFaultInjection, EverySingleByteCorruptionOfAStreamIsAnsweredWithoutGuessing) {
  const Stream stream = stream_of_samples();
  std::mt19937 random(20261020);
  std::uniform_int_distribution<int> byte(0, 255);
  std::map<DecodeFault, std::size_t> faults;
  std::size_t decoded_differently = 0;
  for (std::size_t position = 0; position < stream.bytes.size(); ++position) {
    // Every bit of the byte flipped alone, and one random replacement.
    std::array<std::byte, 9> replacements{};
    for (unsigned bit = 0; bit < 8; ++bit) {
      replacements.at(bit) = stream.bytes[position] ^ static_cast<std::byte>(1U << bit);
    }
    replacements[8] = static_cast<std::byte>(byte(random));
    for (const std::byte replacement : replacements) {
      if (replacement == stream.bytes[position]) {
        continue;
      }
      Bytes corrupted = stream.bytes;
      corrupted[position] = replacement;
      const std::optional<DecodeFault> fault = check_corrupted_stream(stream, corrupted, position);
      if (fault.has_value()) {
        ++faults[*fault];
      } else {
        ++decoded_differently;
      }
    }
  }
  // Every fault a corrupted record can produce was produced: the decoder
  // tells them apart rather than folding them into one.
  for (const DecodeFault fault :
       {DecodeFault::kVersion, DecodeFault::kKind, DecodeFault::kTruncated, DecodeFault::kTrailing,
        DecodeFault::kInvalid}) {
    EXPECT_GT(faults[fault], 0U) << describe(fault);
  }
  EXPECT_EQ(faults[DecodeFault::kEmpty], 0U) << "no corruption of a byte makes a record empty";
  // Value bytes -- an iteration, a seed, a character of a check -- carry no
  // redundancy to break, and their corruptions decode to the record their
  // bytes encode, which the loop above has checked is canonical. That is why
  // the frame carries a checksum (record.hpp); it is not the decoder's to add.
  EXPECT_GT(decoded_differently, 0U);
  RecordProperty("refused",
                 static_cast<int>(faults[DecodeFault::kVersion] + faults[DecodeFault::kKind] +
                                  faults[DecodeFault::kTruncated] + faults[DecodeFault::kTrailing] +
                                  faults[DecodeFault::kInvalid]));
  RecordProperty("decoded_to_their_own_bytes", static_cast<int>(decoded_differently));
}

TEST(RecordDecodeFaultInjection, TheHeaderBytesAreRefusedAsWhatTheyAre) {
  const Stream stream = stream_of_samples();
  for (std::size_t index = 0; index + 1 < stream.starts.size(); ++index) {
    Bytes record(record_at(stream.bytes, stream, index).begin(),
                 record_at(stream.bytes, stream, index).end());
    for (unsigned value = 0; value < 256; ++value) {
      Bytes changed = record;
      changed[0] = static_cast<std::byte>(value);
      if (value != kRecordFormatVersion) {
        EXPECT_EQ(decode(changed).error().fault, DecodeFault::kVersion) << value;
      }
      changed = record;
      changed[1] = static_cast<std::byte>(value);
      if (value == 0 || value > 3) {
        EXPECT_EQ(decode(changed).error().fault, DecodeFault::kKind) << value;
      }
    }
  }
}

TEST(RecordDecodeFaultInjection, MisalignedBoundariesAreAnsweredNeverCrashedOn) {
  // A reader out of step with the records: every window of the stream that is
  // not one record's bytes. Whatever decodes must still be canonical.
  const Stream stream = stream_of_samples();
  std::size_t answered = 0;
  for (std::size_t start = 0; start < stream.bytes.size(); start += 3) {
    for (const std::size_t length : {std::size_t{2}, std::size_t{40}, std::size_t{171}}) {
      if (start + length > stream.bytes.size()) {
        continue;
      }
      const auto window = std::span<const std::byte>(stream.bytes).subspan(start, length);
      const auto decoded = decode(window);
      if (decoded.has_value()) {
        const Bytes again = encode(decoded.value());
        EXPECT_TRUE(std::equal(again.begin(), again.end(), window.begin(), window.end()));
      }
      ++answered;
    }
  }
  EXPECT_GT(answered, stream.bytes.size() / 3);
}

TEST(RecordDecodeFaultInjection, RandomBytesUnderAValidHeaderAreNeverAGuess) {
  // The contract suite's noise, deeper: every kind, lengths up to one record,
  // and the canonical property rather than validity alone.
  std::size_t decoded = 0;
  for (std::uint64_t stream = 0; stream < 20000; ++stream) {
    Bytes bytes(2 + stream % 150);
    rng::fill_stream(0xC0DEC, stream, bytes);
    bytes[0] = std::byte{kRecordFormatVersion};
    bytes[1] = static_cast<std::byte>(1 + stream % 3);
    // Bias the presence flags towards 0 and 1, or nothing gets past them.
    for (std::size_t at = 2; at < bytes.size() && at < 14; ++at) {
      bytes[at] &= std::byte{0x01};
    }
    const auto result = decode(bytes);
    if (result.has_value()) {
      ++decoded;
      EXPECT_TRUE(is_valid(result.value()));
      EXPECT_EQ(encode(result.value()), bytes) << "stream " << stream;
    }
  }
  RecordProperty("decoded", static_cast<int>(decoded));
}

}  // namespace
}  // namespace loadforge::verification

namespace loadforge::contracts {
// The samples are the suite's; the suite itself runs in the unit tier.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RecordCodecContract);
}  // namespace loadforge::contracts
