// SPDX-License-Identifier: GPL-3.0-or-later
//
// JournalContract against this stream's implementation over memory (the suite
// brings its own Storage), and the writer and replay behaviours the suite
// leaves to the implementation.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/journal/journal_contract.hpp"
#include "controller/journal/format.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {
namespace {

struct Implementation {
  static core::Result<std::unique_ptr<Writer>, JournalError> open_writer(Storage& storage) {
    return journal::open_writer(storage);
  }
  static core::Result<Replay, JournalError> replay(const Storage& storage) {
    return journal::replay(storage);
  }
};

}  // namespace
}  // namespace loadforge::journal

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Journal, JournalContract,
                               ::testing::Types<journal::Implementation>);
}  // namespace loadforge::contracts

namespace loadforge::journal {

namespace {

using contracts::journal_suite::MemoryStorage;

TEST(JournalWriter, ARefusedOpenReportsTheReadFailure) {
  MemoryStorage storage;
  storage.fail_read = true;
  const auto writer = open_writer(storage);
  ASSERT_FALSE(writer.has_value());
  EXPECT_EQ(writer.error(), (JournalError{JournalFault::kStorage, "EIO"}));
}

TEST(JournalWriter, AFailedTerminatorLeavesTheWriterOpen) {
  MemoryStorage storage;
  auto writer = open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  storage.fail_append = true;
  EXPECT_EQ(writer.value()->close().error().fault, JournalFault::kStorage);
  storage.fail_append = false;
  EXPECT_TRUE(writer.value()->append(contracts::journal_suite::entries()[0]).has_value())
      << "a close that did not finish is not a close";
}

TEST(JournalWriter, AFailedSyncAfterTheTerminatorStillClosesTheWriter) {
  MemoryStorage storage;
  auto writer = open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  storage.fail_sync = true;
  EXPECT_EQ(writer.value()->close().error(), (JournalError{JournalFault::kStorage, "EIO"}));
  EXPECT_EQ(writer.value()->close().error().fault, JournalFault::kClosed);
  EXPECT_EQ(replay(storage).value().ending, Ending::kTerminated)
      << "the terminator is in the storage whether or not it was synced";
}

TEST(JournalWriter, ATooLongLabelNamesNeitherCallNorStorage) {
  MemoryStorage storage;
  auto writer = open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  Entry entry;
  entry.phase = std::string(kMaxLabelBytes + 1, 'p');
  const auto refused = writer.value()->append(entry);
  EXPECT_EQ(refused.error().fault, JournalFault::kTooLarge);
  EXPECT_TRUE(storage.bytes().empty());
}

TEST(JournalReplay, AFailedAppendMidJournalMakesEverythingAfterItCorrupt) {
  MemoryStorage storage;
  auto writer = open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  const auto entries = contracts::journal_suite::entries();
  ASSERT_TRUE(writer.value()->append(entries[0]).has_value());
  storage.fail_append = true;  // the memory storage keeps half the record
  ASSERT_FALSE(writer.value()->append(entries[1]).has_value());
  storage.fail_append = false;
  ASSERT_TRUE(writer.value()->append(entries[2]).has_value());
  const Replay replay_after = replay(storage).value();
  EXPECT_EQ(replay_after.entries, std::vector<Entry>{entries[0]});
  EXPECT_EQ(replay_after.ending, Ending::kCorrupt);
}

TEST(JournalReplay, AnEntryWithFullLabelsIsAboutTwoHundredBytes) {
  MemoryStorage storage;
  auto writer = open_writer(storage);
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer.value()->append(contracts::journal_suite::entries()[2]).has_value());
  EXPECT_GE(storage.bytes().size(), 150U);
  EXPECT_LE(storage.bytes().size(), 250U);
}

}  // namespace
}  // namespace loadforge::journal
