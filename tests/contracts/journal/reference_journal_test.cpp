// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference journal, run against the journal suite, and the
// definitions the journal header carries. Records are a length, a CRC-32 over
// the length and the payload, and the payload; the journal stream designs the
// real format.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/journal/journal_contract.hpp"
#include "controller/journal/journal.hpp"
#include "core/crc32.hpp"

namespace loadforge::contracts {
namespace {

using journal::Ending;
using journal::Entry;
using journal::JournalError;
using journal::JournalFault;

constexpr std::uint8_t kEntryType = 1;
constexpr std::uint8_t kTerminatorType = 2;

void put64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>(value >> shift));
  }
}

std::uint64_t get64(std::span<const std::byte> in, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value |= std::to_integer<std::uint64_t>(in[at + byte]) << (8 * byte);
  }
  return value;
}

std::vector<std::byte> record(std::span<const std::byte> payload) {
  std::vector<std::byte> out;
  put64(out, payload.size());
  std::vector<std::byte> checked = out;
  checked.insert(checked.end(), payload.begin(), payload.end());
  put64(out, core::crc32(checked));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::byte> encode(const Entry& entry) {
  std::vector<std::byte> out{std::byte{kEntryType}};
  put64(out, static_cast<std::uint64_t>(entry.elapsed_ns));
  for (const std::string* label : {&entry.phase, &entry.workload}) {
    out.push_back(static_cast<std::byte>(label->size()));
    for (const char c : *label) {
      out.push_back(static_cast<std::byte>(c));
    }
  }
  const auto put_optional = [&out](std::optional<std::uint64_t> value) {
    out.push_back(std::byte{value.has_value() ? std::uint8_t{1} : std::uint8_t{0}});
    put64(out, value.value_or(0));
  };
  const auto bits = [](std::optional<double> value) -> std::optional<std::uint64_t> {
    if (!value.has_value()) {
      return std::nullopt;
    }
    return std::bit_cast<std::uint64_t>(*value);
  };
  put_optional(entry.last_verified_unit);
  put_optional(bits(entry.temperature_c));
  put_optional(bits(entry.package_power_w));
  put_optional(bits(entry.frequency_hz));
  put_optional(entry.ecc_corrected);
  put_optional(entry.ecc_uncorrected);
  put64(out, entry.workers_alive);
  put64(out, entry.workers_stale);
  return out;
}

std::optional<Entry> decode(std::span<const std::byte> payload) {
  Entry entry;
  std::size_t at = 1;
  const auto need = [&](std::size_t bytes) { return payload.size() - at >= bytes; };
  if (!need(8)) {
    return std::nullopt;
  }
  entry.elapsed_ns = static_cast<std::int64_t>(get64(payload, at));
  at += 8;
  for (std::string* label : {&entry.phase, &entry.workload}) {
    if (!need(1)) {
      return std::nullopt;
    }
    const auto size = std::to_integer<std::size_t>(payload[at++]);
    if (!need(size)) {
      return std::nullopt;
    }
    for (std::size_t c = 0; c < size; ++c) {
      label->push_back(static_cast<char>(payload[at++]));
    }
  }
  std::vector<std::optional<std::uint64_t>> optionals;
  for (int field = 0; field < 6; ++field) {
    if (!need(9)) {
      return std::nullopt;
    }
    const bool present = payload[at] != std::byte{0};
    optionals.push_back(present ? std::optional(get64(payload, at + 1)) : std::nullopt);
    at += 9;
  }
  if (payload.size() - at != 16) {
    return std::nullopt;
  }
  const auto real = [](std::optional<std::uint64_t> raw) -> std::optional<double> {
    if (!raw.has_value()) {
      return std::nullopt;
    }
    return std::bit_cast<double>(*raw);
  };
  entry.last_verified_unit = optionals[0];
  entry.temperature_c = real(optionals[1]);
  entry.package_power_w = real(optionals[2]);
  entry.frequency_hz = real(optionals[3]);
  entry.ecc_corrected = optionals[4];
  entry.ecc_uncorrected = optionals[5];
  entry.workers_alive = static_cast<std::uint32_t>(get64(payload, at));
  entry.workers_stale = static_cast<std::uint32_t>(get64(payload, at + 8));
  return entry;
}

class ReferenceWriter final : public journal::Writer {
 public:
  explicit ReferenceWriter(journal::Storage& storage) : storage_(&storage) {}
  core::Result<core::Ok, JournalError> append(const Entry& entry) override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "append after close"};
    }
    if (entry.phase.size() > journal::kMaxLabelBytes ||
        entry.workload.size() > journal::kMaxLabelBytes) {
      return JournalError{JournalFault::kTooLarge, "a label is too long"};
    }
    return storage_->append(record(encode(entry)));
  }
  core::Result<core::Ok, JournalError> sync() override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "sync after close"};
    }
    return storage_->sync();
  }
  core::Result<core::Ok, JournalError> close() override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "close after close"};
    }
    const std::vector<std::byte> terminator{std::byte{kTerminatorType}};
    auto appended = storage_->append(record(terminator));
    if (!appended.has_value()) {
      return appended;
    }
    closed_ = true;
    return storage_->sync();
  }

 private:
  journal::Storage* storage_;
  bool closed_ = false;
};

core::Result<journal::Replay, JournalError> reference_replay(const journal::Storage& storage) {
  auto read = storage.read_all();
  if (!read.has_value()) {
    return read.error();
  }
  const std::vector<std::byte>& bytes = read.value();
  journal::Replay replay;
  if (bytes.empty()) {
    return replay;
  }
  std::size_t at = 0;
  while (true) {
    if (at == bytes.size()) {
      replay.ending = Ending::kUnterminated;
      return replay;
    }
    if (bytes.size() - at < 16) {
      replay.ending = Ending::kTornTail;
      return replay;
    }
    const std::uint64_t length = get64(bytes, at);
    if (bytes.size() - at - 16 < length) {
      replay.ending = Ending::kTornTail;
      return replay;
    }
    std::vector<std::byte> checked(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(at + 8));
    const auto payload = std::span(bytes).subspan(at + 16, length);
    checked.insert(checked.end(), payload.begin(), payload.end());
    if (core::crc32(checked) != get64(bytes, at + 8) || payload.empty()) {
      replay.ending = Ending::kCorrupt;
      return replay;
    }
    at += 16 + length;
    if (payload[0] == std::byte{kTerminatorType} && payload.size() == 1) {
      replay.ending = at == bytes.size() ? Ending::kTerminated : Ending::kCorrupt;
      return replay;
    }
    const std::optional<Entry> entry =
        payload[0] == std::byte{kEntryType} ? decode(payload) : std::nullopt;
    if (!entry.has_value()) {
      replay.ending = Ending::kCorrupt;
      return replay;
    }
    replay.entries.push_back(*entry);
  }
}

struct ReferenceJournal {
  static core::Result<std::unique_ptr<journal::Writer>, JournalError> open_writer(
      journal::Storage& storage) {
    auto read = storage.read_all();
    if (!read.has_value()) {
      return read.error();
    }
    if (!read.value().empty()) {
      return JournalError{JournalFault::kNotEmpty, "the storage holds a journal"};
    }
    return std::unique_ptr<journal::Writer>(std::make_unique<ReferenceWriter>(storage));
  }
  static core::Result<journal::Replay, JournalError> replay(const journal::Storage& storage) {
    return reference_replay(storage);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, JournalContract, ::testing::Types<ReferenceJournal>);

TEST(JournalDefinition, FaultAndEndingWords) {
  using journal::describe;
  EXPECT_EQ(describe(JournalFault::kStorage), "storage failed");
  EXPECT_EQ(describe(JournalFault::kTooLarge), "entry too large");
  EXPECT_EQ(describe(JournalFault::kNotEmpty), "journal already written");
  EXPECT_EQ(describe(JournalFault::kClosed), "journal closed");
  EXPECT_EQ(describe(static_cast<JournalFault>(9)), "unrecognised journal fault");
  EXPECT_EQ(describe(Ending::kEmpty), "empty");
  EXPECT_EQ(describe(Ending::kTerminated), "ended cleanly");
  EXPECT_EQ(describe(Ending::kUnterminated), "ended abnormally between records");
  EXPECT_EQ(describe(Ending::kTornTail), "ended abnormally inside a record");
  EXPECT_EQ(describe(Ending::kCorrupt), "corrupt");
  EXPECT_EQ(describe(static_cast<Ending>(9)), "unrecognised ending");
}

TEST(JournalEquality, EveryMemberIsCompared) {
  const Entry entry = journal_suite::entries()[1];
  expect_memberwise_equality(
      entry,
      {[](Entry& e) { e.elapsed_ns = 1; }, [](Entry& e) { e.phase = "x"; },
       [](Entry& e) { e.workload = "x"; }, [](Entry& e) { e.last_verified_unit.reset(); },
       [](Entry& e) { e.temperature_c.reset(); }, [](Entry& e) { e.package_power_w.reset(); },
       [](Entry& e) { e.frequency_hz.reset(); }, [](Entry& e) { e.ecc_corrected.reset(); },
       [](Entry& e) { e.ecc_uncorrected.reset(); }, [](Entry& e) { e.workers_alive = 9; },
       [](Entry& e) { e.workers_stale = 9; }});
  expect_memberwise_equality(JournalError{JournalFault::kStorage, "d"},
                             {[](JournalError& e) { e.fault = JournalFault::kClosed; },
                              [](JournalError& e) { e.detail = "e"; }});
  expect_memberwise_equality(journal::Replay{{entry}, Ending::kTerminated},
                             {[](journal::Replay& r) { r.entries.clear(); },
                              [](journal::Replay& r) { r.ending = Ending::kCorrupt; }});
}

}  // namespace
}  // namespace loadforge::contracts
