// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/journal/format.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

#include "core/crc32.hpp"

namespace loadforge::journal {
namespace {

using format::decode_entry;
using format::encode_entry;
using format::frame;
using format::scan;

std::vector<std::byte> bytes_of(const std::string& hex) {
  std::vector<std::byte> out;
  for (std::size_t at = 0; at < hex.size(); at += 2) {
    out.push_back(static_cast<std::byte>(std::stoi(hex.substr(at, 2), nullptr, 16)));
  }
  return out;
}

Entry sample() {
  Entry entry;
  entry.elapsed_ns = 1;
  entry.phase = "a";
  entry.workload = "bc";
  entry.last_verified_unit = 7;
  entry.temperature_c = 1.5;
  entry.workers_alive = 2;
  entry.workers_stale = 3;
  return entry;
}

std::vector<std::byte> record_of(const Entry& entry) { return frame(encode_entry(entry)); }

std::vector<std::byte> concat(std::vector<std::byte> first, const std::vector<std::byte>& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

// The bytes below come from Python's zlib.crc32 and struct.pack, run on the
// layout format.hpp documents (python3 3.12, x86-64): an independent reading
// of the same format, not this code's own output (F21).
TEST(JournalFormat, TheTerminatorRecordIsTheBytesAnOutsideReadingGives) {
  EXPECT_EQ(frame(format::terminator_payload()), bytes_of("0100000081bf4c1502"));
}

TEST(JournalFormat, AnEntryRecordIsTheBytesAnOutsideReadingGives) {
  EXPECT_EQ(record_of(sample()),
            bytes_of("470000008324d3d00101000000000000000301610262630700000000000000000000000000"
                     "f83f000000000000000000000000000000000000000000000000000000000000000002000000"
                     "03000000"));
}

TEST(JournalFormat, EveryCombinationOfPresentFieldsRoundTrips) {
  for (unsigned mask = 0; mask < 64; ++mask) {
    Entry entry = sample();
    entry.last_verified_unit = (mask & 1U) != 0 ? std::optional<std::uint64_t>(1) : std::nullopt;
    entry.temperature_c = (mask & 2U) != 0 ? std::optional<double>(-0.0) : std::nullopt;
    entry.package_power_w = (mask & 4U) != 0
                                ? std::optional<double>(std::numeric_limits<double>::denorm_min())
                                : std::nullopt;
    entry.frequency_hz = (mask & 8U) != 0 ? std::optional<double>(3.6e9) : std::nullopt;
    entry.ecc_corrected =
        (mask & 16U) != 0 ? std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max())
                          : std::nullopt;
    entry.ecc_uncorrected = (mask & 32U) != 0 ? std::optional<std::uint64_t>(0) : std::nullopt;
    entry.elapsed_ns = std::numeric_limits<std::int64_t>::min();
    const auto payload = encode_entry(entry);
    const auto decoded = decode_entry(payload);
    ASSERT_TRUE(decoded.has_value()) << mask;
    EXPECT_EQ(*decoded, entry) << mask;
    if (entry.temperature_c.has_value()) {
      EXPECT_TRUE(std::signbit(*decoded->temperature_c)) << "negative zero survives";
    }
  }
}

TEST(JournalFormat, AnEntryWithNothingOptionalIsStillAFixedSize) {
  const Entry plain;
  EXPECT_EQ(encode_entry(plain).size(), 1U + 8 + 1 + 1 + 1 + 48 + 8);
  EXPECT_LE(encode_entry(plain).size() + 2 * kMaxLabelBytes, format::kMaxPayloadBytes);
}

TEST(JournalFormat, APayloadCutAtAnyLengthIsNotAnEntry) {
  Entry entry = sample();
  entry.phase = std::string(kMaxLabelBytes, 'p');
  const auto payload = encode_entry(entry);
  for (std::size_t cut = 0; cut < payload.size(); ++cut) {
    EXPECT_FALSE(decode_entry(std::span(payload).first(cut)).has_value()) << cut;
  }
  EXPECT_TRUE(decode_entry(payload).has_value());
}

TEST(JournalFormat, BytesAfterTheLastFieldAreNotAnEntry) {
  auto payload = encode_entry(sample());
  payload.push_back(std::byte{0});
  EXPECT_FALSE(decode_entry(payload).has_value());
}

TEST(JournalFormat, AReservedPresenceBitIsNotAnEntry) {
  for (unsigned bit = 6; bit < 8; ++bit) {
    auto payload = encode_entry(sample());
    payload[9] |= static_cast<std::byte>(1U << bit);
    EXPECT_FALSE(decode_entry(payload).has_value()) << bit;
  }
}

TEST(JournalFormat, ALabelPastTheLimitIsNotAnEntry) {
  for (const std::size_t label : {std::size_t{10}, std::size_t{12}}) {
    auto payload = encode_entry(sample());
    payload[label] = static_cast<std::byte>(kMaxLabelBytes + 1);
    EXPECT_FALSE(decode_entry(payload).has_value()) << label;
  }
}

TEST(JournalScan, NoBytesIsEmpty) {
  const Replay replay = scan({});
  EXPECT_TRUE(replay.entries.empty());
  EXPECT_EQ(replay.ending, Ending::kEmpty);
}

TEST(JournalScan, EntriesWithoutATerminatorEndBetweenRecords) {
  const auto bytes = concat(record_of(sample()), record_of(sample()));
  const Replay replay = scan(bytes);
  EXPECT_EQ(replay.entries.size(), 2U);
  EXPECT_EQ(replay.ending, Ending::kUnterminated);
}

TEST(JournalScan, AHeaderCutShortIsATornTail) {
  const auto whole = record_of(sample());
  for (std::size_t cut = 1; cut < format::kHeaderBytes; ++cut) {
    EXPECT_EQ(scan(std::span(whole).first(cut)).ending, Ending::kTornTail) << cut;
  }
}

TEST(JournalScan, APayloadCutShortIsATornTailAfterTheRecordsBeforeIt) {
  const auto first = record_of(sample());
  const auto second = record_of(sample());
  const auto bytes = concat(first, second);
  const Replay replay = scan(std::span(bytes).first(bytes.size() - 1));
  EXPECT_EQ(replay.entries.size(), 1U);
  EXPECT_EQ(replay.ending, Ending::kTornTail);
}

void set_length(std::vector<std::byte>& record, std::uint32_t length) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    record[byte] = static_cast<std::byte>(length >> (8 * byte));
  }
}

TEST(JournalScan, ALengthOfZeroOrPastTheMaximumIsCorrupt) {
  for (const std::uint32_t length : {0U, 513U, 0xffffffffU}) {
    auto bytes = record_of(sample());
    set_length(bytes, length);
    EXPECT_EQ(scan(bytes).ending, Ending::kCorrupt) << length;
  }
}

TEST(JournalScan, TheMaximumLengthIsTornWhenTheBytesRunOut) {
  auto bytes = record_of(sample());
  set_length(bytes, static_cast<std::uint32_t>(format::kMaxPayloadBytes));
  EXPECT_EQ(scan(bytes).ending, Ending::kTornTail);
}

TEST(JournalScan, AFailedCheckIsCorruptAndKeepsEarlierEntries) {
  auto bytes = concat(record_of(sample()), record_of(sample()));
  bytes.back() ^= std::byte{1};
  const Replay replay = scan(bytes);
  EXPECT_EQ(replay.entries.size(), 1U);
  EXPECT_EQ(replay.ending, Ending::kCorrupt);
}

TEST(JournalScan, ARecordWithAGoodCheckThatIsNotAnEntryIsCorrupt) {
  const std::vector<std::byte> unknown_type{std::byte{9}};
  EXPECT_EQ(scan(frame(unknown_type)).ending, Ending::kCorrupt);
  const std::vector<std::byte> bare_entry{format::kEntry};
  EXPECT_EQ(scan(frame(bare_entry)).ending, Ending::kCorrupt);
  const std::vector<std::byte> long_terminator{format::kTerminator, std::byte{0}};
  EXPECT_EQ(scan(frame(long_terminator)).ending, Ending::kCorrupt);
}

TEST(JournalScan, BytesAfterTheTerminatorAreCorrupt) {
  const auto closed = concat(record_of(sample()), frame(format::terminator_payload()));
  EXPECT_EQ(scan(closed).ending, Ending::kTerminated);
  EXPECT_EQ(scan(concat(closed, record_of(sample()))).ending, Ending::kCorrupt);
  EXPECT_EQ(scan(concat(closed, {std::byte{0}})).ending, Ending::kCorrupt);
}

TEST(JournalScan, TheCheckCoversTheLengthBytes) {
  // A record whose check was computed over its payload alone must be refused.
  auto bytes = record_of(sample());
  const auto payload = std::span(bytes).subspan(format::kHeaderBytes);
  const std::uint32_t over_payload_only = core::crc32(payload);
  for (unsigned byte = 0; byte < 4; ++byte) {
    bytes[4 + byte] = static_cast<std::byte>(over_payload_only >> (8 * byte));
  }
  EXPECT_EQ(scan(bytes).ending, Ending::kCorrupt);
}

}  // namespace
}  // namespace loadforge::journal
