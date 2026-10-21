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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "contracts/verification/record_contract.hpp"
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
    const Bytes record(record_at(stream.bytes, stream, index).begin(),
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

/// A random valid record: every field present or absent at random, random
/// values, texts and windows of random lengths, every kind and enumerator.
ErrorRecord random_record(std::mt19937_64& random) {
  const auto coin = [&random] { return (random() & 1U) != 0; };
  const auto text = [&random](std::size_t longest) {
    std::string value(random() % (longest + 1), ' ');
    for (char& c : value) {
      c = static_cast<char>(random() % 256);
    }
    return value;
  };
  Provenance provenance;
  if (coin()) {
    provenance.logical_cpu = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.physical_core = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.numa_node = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.physical_package = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.decomposition = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.footprint_bytes = random();
  }
  if (coin()) {
    provenance.buffer_address = random();
  }
  if (coin()) {
    provenance.buffer_offset = random();
  }
  if (coin()) {
    provenance.iteration = random();
  }
  if (coin()) {
    provenance.workload = text(40);
  }
  if (coin()) {
    provenance.workload_version = static_cast<std::uint32_t>(random());
  }
  if (coin()) {
    provenance.seed = random();
  }
  if (coin()) {
    provenance.isa = static_cast<simd::IsaPath>(random() % simd::kIsaPathCount);
  }
  if (coin()) {
    provenance.input_integrity = static_cast<InputIntegrity>(random() % 4);
  }
  if (coin()) {
    provenance.duty =
        DutyCycle{static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random())};
  }
  switch (random() % 3) {
    case 0: {
      ExactBitCorruption record;
      record.provenance = provenance;
      record.side = static_cast<Side>(random() % 2);
      record.check = "c" + text(40);
      record.line_bytes = 1U << (3 + random() % 10);
      if (coin()) {
        Difference difference;
        difference.domain = static_cast<Domain>(random() % 2);
        difference.first_offset = random() >> 8U;
        difference.last_offset = difference.first_offset + random() % 5000;
        difference.popcount = 1 + random() % 100;
        difference.window_offset = difference.first_offset / kWordBytes * kWordBytes;
        const std::size_t first_at = difference.first_offset - difference.window_offset;
        const std::size_t window = first_at + 1 + random() % (kMaxWindowBytes - first_at);
        for (std::size_t at = 0; at < window; ++at) {
          difference.expected.push_back(static_cast<std::byte>(random()));
          difference.actual.push_back(static_cast<std::byte>(random()));
        }
        // A valid difference differs where it says it does (W1-05).
        const auto make_differ = [&difference](std::size_t at) {
          if (difference.expected[at] == difference.actual[at]) {
            difference.actual[at] = ~difference.expected[at];
          }
        };
        make_differ(first_at);
        if (difference.last_offset - difference.window_offset < window) {
          make_differ(difference.last_offset - difference.window_offset);
        }
        record.difference = difference;
      }
      return record;
    }
    case 1: {
      NumericalVerificationFailure record;
      record.provenance = provenance;
      record.check = text(40);
      record.tolerance = 1e-300 + static_cast<double>(random() % 1000000);
      // A failure: a NaN of any sign and payload, or a residual past the
      // tolerance (W1-01).
      record.residual = std::fabs(std::bit_cast<double>(random()));
      if (coin()) {
        record.residual = std::bit_cast<double>(random() | 0x7FF0000000000001ULL);
      } else if (!(record.residual > record.tolerance)) {
        record.residual = record.tolerance * static_cast<double>(2 + random() % 10);
      }
      if (coin()) {
        record.condition_number = std::bit_cast<double>(random());
      }
      if (coin()) {
        record.element = random();
      }
      return record;
    }
    default: {
      VerificationNotPerformed record;
      record.provenance = provenance;
      record.what = text(40);
      record.why = text(40);
      record.blocks_compared = random();
      return record;
    }
  }
}

TEST(RecordDecodeFaultInjection, RandomRecordsRoundTripAndTheirCorruptionsAreNeverAGuess) {
  std::mt19937_64 random(20261020);
  std::size_t refused = 0;
  std::size_t decoded = 0;
  for (int round = 0; round < 5000; ++round) {
    const ErrorRecord record = random_record(random);
    ASSERT_TRUE(is_valid(record)) << round;
    const Bytes bytes = encode(record);
    const auto back = decode(bytes);
    ASSERT_TRUE(back.has_value()) << round << ": " << describe(back.error());
    EXPECT_TRUE(contracts::record_samples::same(back.value(), record)) << round;
    // Then one byte, anywhere, overwritten with anything.
    Bytes corrupted = bytes;
    corrupted[random() % corrupted.size()] = static_cast<std::byte>(random());
    const auto answer = decode(corrupted);
    if (!answer.has_value()) {
      ++refused;
      continue;
    }
    ++decoded;
    EXPECT_TRUE(is_valid(answer.value())) << round;
    EXPECT_EQ(encode(answer.value()), corrupted) << round << ": a guess";
  }
  EXPECT_GT(refused, 0U);
  EXPECT_GT(decoded, 0U);
  RecordProperty("refused", static_cast<int>(refused));
  RecordProperty("decoded_to_their_own_bytes", static_cast<int>(decoded));
}

}  // namespace
}  // namespace loadforge::verification

namespace loadforge::contracts {
// The samples are the suite's; the suite itself runs in the unit tier.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RecordCodecContract);
}  // namespace loadforge::contracts
