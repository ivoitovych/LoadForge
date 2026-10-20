// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_JOURNAL_JOURNAL_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_JOURNAL_JOURNAL_CONTRACT_HPP

// Provider-side suite for src/controller/journal/journal.hpp (ADR 0003, 0004).
//
// JournalContract<Subject>:
//   Subject::open_writer(Storage&)   as journal::open_writer.
//   Subject::replay(const Storage&)  as journal::replay.
//
// The suite brings its own Storage over memory, which it cuts and corrupts at
// every byte and makes fail on demand: the machine dying between any two
// bytes is the event this contract exists for.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "controller/journal/journal.hpp"

namespace loadforge::contracts {

namespace journal_suite {

class MemoryStorage final : public journal::Storage {
 public:
  MemoryStorage() = default;
  explicit MemoryStorage(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

  core::Result<core::Ok, journal::JournalError> append(std::span<const std::byte> bytes) override {
    if (fail_append) {
      bytes_.insert(bytes_.end(), bytes.begin(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
      return journal::JournalError{journal::JournalFault::kStorage, "ENOSPC"};
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return core::Ok{};
  }
  core::Result<core::Ok, journal::JournalError> sync() override {
    if (fail_sync) {
      return journal::JournalError{journal::JournalFault::kStorage, "EIO"};
    }
    ++syncs;
    return core::Ok{};
  }
  [[nodiscard]] core::Result<std::vector<std::byte>, journal::JournalError> read_all()
      const override {
    if (fail_read) {
      return journal::JournalError{journal::JournalFault::kStorage, "EIO"};
    }
    return bytes_;
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

  bool fail_append = false;
  bool fail_sync = false;
  bool fail_read = false;
  int syncs = 0;

 private:
  std::vector<std::byte> bytes_;
};

inline std::vector<journal::Entry> entries() {
  journal::Entry first;
  first.elapsed_ns = 5'000'000'000;
  first.phase = "warm-up";
  first.workload = "compression";
  first.workers_alive = 4;
  journal::Entry second = first;
  second.elapsed_ns = 10'000'000'000;
  second.phase = "stress";
  second.last_verified_unit = 1234;
  second.temperature_c = 81.5;
  second.package_power_w = 95.25;
  second.frequency_hz = 3.6e9;
  second.ecc_corrected = 3;
  second.ecc_uncorrected = 0;
  second.workers_stale = 1;
  journal::Entry third = second;
  third.elapsed_ns = 15'000'000'000;
  third.phase = std::string(journal::kMaxLabelBytes, 'p');
  third.workload = std::string(journal::kMaxLabelBytes, 'w');
  return {first, second, third};
}

}  // namespace journal_suite

template <typename Subject>
class JournalContract : public ::testing::Test {
 protected:
  /// A journal of the suite's three entries, closed when `close` says so.
  static std::vector<std::byte> written(bool close) {
    journal_suite::MemoryStorage storage;
    auto writer = Subject::open_writer(storage);
    EXPECT_TRUE(writer.has_value());
    for (const journal::Entry& entry : journal_suite::entries()) {
      EXPECT_TRUE(writer.value()->append(entry).has_value());
    }
    if (close) {
      EXPECT_TRUE(writer.value()->close().has_value());
    }
    return storage.bytes();
  }

  static journal::Replay replayed(std::vector<std::byte> bytes) {
    const journal_suite::MemoryStorage storage(std::move(bytes));
    const auto replay = Subject::replay(storage);
    if (!replay.has_value()) {
      ADD_FAILURE() << replay.error().detail;
      return journal::Replay{};
    }
    return replay.value();
  }

  static bool is_prefix(const std::vector<journal::Entry>& entries) {
    const std::vector<journal::Entry> all = journal_suite::entries();
    return entries.size() <= all.size() && std::equal(entries.begin(), entries.end(), all.begin());
  }
};
TYPED_TEST_SUITE_P(JournalContract);

using journal::Ending;

TYPED_TEST_P(JournalContract, AClosedJournalReplaysEveryEntryAndEndsCleanly) {
  const journal::Replay replay = this->replayed(this->written(true));
  EXPECT_EQ(replay.entries, journal_suite::entries());
  EXPECT_EQ(replay.ending, Ending::kTerminated);
}

TYPED_TEST_P(JournalContract, AJournalNeverClosedEndedAbnormally) {
  const journal::Replay replay = this->replayed(this->written(false));
  EXPECT_EQ(replay.entries, journal_suite::entries());
  EXPECT_EQ(replay.ending, Ending::kUnterminated);
}

TYPED_TEST_P(JournalContract, NoBytesIsAnEmptyJournal) {
  const journal::Replay replay = this->replayed({});
  EXPECT_TRUE(replay.entries.empty());
  EXPECT_EQ(replay.ending, Ending::kEmpty);
}

TYPED_TEST_P(JournalContract, ACutAtAnyByteKeepsEveryWholeRecordBeforeIt) {
  const std::vector<std::byte> whole = this->written(true);
  std::size_t previous = 0;
  for (std::size_t cut = 0; cut < whole.size(); ++cut) {
    const journal::Replay replay = this->replayed(
        std::vector<std::byte>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut)));
    EXPECT_TRUE(this->is_prefix(replay.entries)) << "cut at " << cut;
    EXPECT_GE(replay.entries.size(), previous) << "a longer journal never loses an entry";
    previous = replay.entries.size();
    EXPECT_NE(replay.ending, Ending::kCorrupt) << "a cut alone is never corruption, at " << cut;
    EXPECT_NE(replay.ending, Ending::kTerminated) << "cut at " << cut;
    if (cut == 0) {
      EXPECT_EQ(replay.ending, Ending::kEmpty);
    }
  }
  EXPECT_EQ(previous, journal_suite::entries().size()) << "only the terminator was cut";
}

TYPED_TEST_P(JournalContract, AChangedByteIsNeverReadAsAnEntry) {
  const std::vector<std::byte> whole = this->written(true);
  for (std::size_t at = 0; at < whole.size(); ++at) {
    for (const std::byte flip : {std::byte{0x01}, std::byte{0x80}}) {
      std::vector<std::byte> damaged = whole;
      damaged[at] ^= flip;
      const journal::Replay replay = this->replayed(damaged);
      EXPECT_TRUE(this->is_prefix(replay.entries)) << "byte " << at;
      EXPECT_FALSE(replay.ending == Ending::kTerminated &&
                   replay.entries.size() == journal_suite::entries().size())
          << "byte " << at << " changed and nothing noticed";
    }
  }
  std::vector<std::byte> trailing = whole;
  trailing.push_back(std::byte{0});
  EXPECT_EQ(this->replayed(trailing).ending, Ending::kCorrupt) << "bytes after the terminator";
}

TYPED_TEST_P(JournalContract, ALabelTooLongIsRefusedAndWritesNothing) {
  journal_suite::MemoryStorage storage;
  auto writer = TypeParam::open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  const std::size_t before = storage.bytes().size();
  journal::Entry entry = journal_suite::entries()[0];
  entry.phase = std::string(journal::kMaxLabelBytes + 1, 'p');
  EXPECT_EQ(writer.value()->append(entry).error().fault, journal::JournalFault::kTooLarge);
  entry = journal_suite::entries()[0];
  entry.workload = std::string(journal::kMaxLabelBytes + 1, 'w');
  EXPECT_EQ(writer.value()->append(entry).error().fault, journal::JournalFault::kTooLarge);
  EXPECT_EQ(storage.bytes().size(), before);
}

TYPED_TEST_P(JournalContract, SyncIsTheStoragesAndCloseSyncs) {
  journal_suite::MemoryStorage storage;
  auto writer = TypeParam::open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer.value()->append(journal_suite::entries()[0]).has_value());
  const int before = storage.syncs;
  ASSERT_TRUE(writer.value()->sync().has_value());
  EXPECT_GT(storage.syncs, before);
  const int synced = storage.syncs;
  ASSERT_TRUE(writer.value()->close().has_value());
  EXPECT_GT(storage.syncs, synced) << "close makes the terminator durable";
}

TYPED_TEST_P(JournalContract, AStorageFailureIsReportedNotSwallowed) {
  journal_suite::MemoryStorage storage;
  auto writer = TypeParam::open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  storage.fail_append = true;
  EXPECT_EQ(writer.value()->append(journal_suite::entries()[0]).error().fault,
            journal::JournalFault::kStorage);
  storage.fail_append = false;
  storage.fail_sync = true;
  EXPECT_EQ(writer.value()->sync().error().fault, journal::JournalFault::kStorage);
  storage.fail_read = true;
  EXPECT_EQ(TypeParam::replay(storage).error().fault, journal::JournalFault::kStorage);
}

TYPED_TEST_P(JournalContract, NothingIsWrittenAfterClose) {
  journal_suite::MemoryStorage storage;
  auto writer = TypeParam::open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer.value()->close().has_value());
  const std::size_t closed = storage.bytes().size();
  EXPECT_EQ(writer.value()->append(journal_suite::entries()[0]).error().fault,
            journal::JournalFault::kClosed);
  EXPECT_EQ(writer.value()->sync().error().fault, journal::JournalFault::kClosed);
  EXPECT_EQ(writer.value()->close().error().fault, journal::JournalFault::kClosed);
  EXPECT_EQ(storage.bytes().size(), closed);
  const journal::Replay replay = this->replayed(storage.bytes());
  EXPECT_TRUE(replay.entries.empty());
  EXPECT_EQ(replay.ending, Ending::kTerminated) << "a run that wrote nothing and ended cleanly";
}

TYPED_TEST_P(JournalContract, AJournalIsNeverContinuedByAnotherRun) {
  journal_suite::MemoryStorage storage(this->written(false));
  const auto writer = TypeParam::open_writer(storage);
  ASSERT_FALSE(writer.has_value());
  EXPECT_EQ(writer.error().fault, journal::JournalFault::kNotEmpty);
}

REGISTER_TYPED_TEST_SUITE_P(JournalContract, AClosedJournalReplaysEveryEntryAndEndsCleanly,
                            AJournalNeverClosedEndedAbnormally, NoBytesIsAnEmptyJournal,
                            ACutAtAnyByteKeepsEveryWholeRecordBeforeIt,
                            AChangedByteIsNeverReadAsAnEntry,
                            ALabelTooLongIsRefusedAndWritesNothing, SyncIsTheStoragesAndCloseSyncs,
                            AStorageFailureIsReportedNotSwallowed, NothingIsWrittenAfterClose,
                            AJournalIsNeverContinuedByAnotherRun);

}  // namespace loadforge::contracts

#endif
