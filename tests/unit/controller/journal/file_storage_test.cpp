// SPDX-License-Identifier: GPL-3.0-or-later
//
// FileStorage over a scripted FileOps: every failure of open, write (short
// writes included), fsync, read and close is forced (P2), and each is asserted
// to arrive as kStorage naming the errno.

#include "controller/journal/file_storage.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "controller/journal/journal.hpp"
#include "fake_file_ops.hpp"

namespace loadforge::journal {
namespace {

using testing::FakeFileOps;
using Call = FakeFileOps::Call;

constexpr const char* kPath = "/var/lib/loadforge/run.journal";

std::vector<std::byte> bytes(const std::string& text) {
  std::vector<std::byte> out;
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

std::unique_ptr<FileStorage> opened(FakeFileOps& ops) {
  auto storage = FileStorage::open(ops, kPath);
  EXPECT_TRUE(storage.has_value());
  return std::move(storage).value();
}

// The errnos each call documents that a test process cannot provoke on demand.
const std::vector<int> kWriteErrors{ENOSPC, EIO, EDQUOT, EFBIG, EBADF, EPERM, ENOMEM};
const std::vector<int> kFsyncErrors{EIO, ENOSPC, EDQUOT, EBADF, EROFS, EINVAL};
const std::vector<int> kOpenErrors{EACCES, ENOENT, EISDIR, EROFS, EMFILE, ENFILE,
                                   ENOSPC, ENOTDIR, ELOOP,  ENOMEM, EDQUOT, EPERM};
const std::vector<int> kReadErrors{EIO, EISDIR, EBADF, ENOMEM};
const std::vector<int> kCloseErrors{EIO, EBADF, ENOSPC, EDQUOT};

void expect_storage_fault(const JournalError& error, int number) {
  EXPECT_EQ(error.fault, JournalFault::kStorage);
  EXPECT_NE(error.detail.find("(errno " + std::to_string(number) + ")"), std::string::npos)
      << error.detail << " should carry errno " << number;
}

TEST(FileStorageOpen, CreatesTheFileAndSyncsItsDirectory) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ASSERT_EQ(ops.opens.size(), 2U);
  EXPECT_EQ(ops.opens[0].first, kPath);
  EXPECT_EQ(ops.opens[0].second, FileOps::Mode::kAppend);
  EXPECT_EQ(ops.opens[1].first, "/var/lib/loadforge");
  EXPECT_EQ(ops.opens[1].second, FileOps::Mode::kDirectory);
  EXPECT_EQ(ops.synced_fds.size(), 1U) << "the directory entry";
  EXPECT_EQ(ops.closed_fds.size(), 1U) << "only the directory is closed";
  EXPECT_TRUE(ops.files.contains(kPath));
}

TEST(FileStorageOpen, ABareFileNameSyncsTheCurrentDirectory) {
  FakeFileOps ops;
  const auto storage = FileStorage::open(ops, "run.journal");
  ASSERT_TRUE(storage.has_value());
  EXPECT_EQ(ops.opens[1].first, ".");
}

TEST(FileStorageOpen, ClosesTheDescriptorItOwnsWhenDestroyed) {
  FakeFileOps ops;
  { const auto storage = opened(ops); }
  EXPECT_TRUE(ops.open_fds.empty());
}

class OpenFailure : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(Errno, OpenFailure, ::testing::ValuesIn(kOpenErrors));

TEST_P(OpenFailure, OfTheJournalFileIsReported) {
  FakeFileOps ops;
  ops.script(Call::kOpen, GetParam());
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), GetParam());
  EXPECT_EQ(ops.opens.size(), 1U) << "nothing else was tried";
}

TEST_P(OpenFailure, OfTheDirectoryClosesTheJournalFileAndIsReported) {
  FakeFileOps ops;
  ops.script(Call::kOpen, FakeFileOps::Step{});  // the journal file opens
  ops.script(Call::kOpen, GetParam());
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), GetParam());
  EXPECT_TRUE(ops.open_fds.empty()) << "the descriptor already opened is not leaked";
}

class FsyncFailure : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(Errno, FsyncFailure, ::testing::ValuesIn(kFsyncErrors));

TEST_P(FsyncFailure, OfTheDirectoryClosesBothAndIsReported) {
  FakeFileOps ops;
  ops.script(Call::kFsync, GetParam());
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), GetParam());
  EXPECT_TRUE(ops.open_fds.empty());
}

TEST_P(FsyncFailure, OfTheJournalIsReportedAndNotRetried) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  const std::size_t before = ops.synced_fds.size();
  ops.script(Call::kFsync, GetParam());
  const auto synced = storage->sync();
  ASSERT_FALSE(synced.has_value());
  expect_storage_fault(synced.error(), GetParam());
  EXPECT_EQ(ops.synced_fds.size(), before + 1) << "a failed fsync is not asked again";
}

class CloseFailure : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(Errno, CloseFailure, ::testing::ValuesIn(kCloseErrors));

TEST_P(CloseFailure, OfTheDirectoryIsReportedAfterAGoodSync) {
  FakeFileOps ops;
  ops.script(Call::kClose, GetParam());
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), GetParam());
}

TEST_P(CloseFailure, OfTheDirectoryIsNotWhatIsReportedWhenTheSyncFailedToo) {
  FakeFileOps ops;
  ops.script(Call::kFsync, EIO);
  ops.script(Call::kClose, GetParam());
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), EIO);
}

TEST_P(CloseFailure, OfTheJournalWhenDestroyedIsSwallowed) {
  FakeFileOps ops;
  {
    const auto storage = opened(ops);
    ops.script(Call::kClose, GetParam());
  }
  EXPECT_TRUE(ops.open_fds.empty());
}

class WriteFailure : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(Errno, WriteFailure, ::testing::ValuesIn(kWriteErrors));

TEST_P(WriteFailure, IsReported) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, GetParam());
  const auto appended = storage->append(bytes("abc"));
  ASSERT_FALSE(appended.has_value());
  expect_storage_fault(appended.error(), GetParam());
  EXPECT_TRUE(ops.files[kPath].empty());
}

TEST_P(WriteFailure, AfterAShortWriteIsReportedAndKeepsThePrefix) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, FakeFileOps::Step{0, 2});
  ops.script(Call::kWrite, GetParam());
  const auto appended = storage->append(bytes("abcdef"));
  ASSERT_FALSE(appended.has_value());
  expect_storage_fault(appended.error(), GetParam());
  EXPECT_EQ(ops.files[kPath], bytes("ab"));
}

TEST(FileStorageAppend, ContinuesAfterEveryShortWrite) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  for (int i = 0; i < 6; ++i) {
    ops.script(Call::kWrite, FakeFileOps::Step{0, 1});
  }
  ASSERT_TRUE(storage->append(bytes("abcdef")).has_value());
  EXPECT_EQ(ops.files[kPath], bytes("abcdef"));
  EXPECT_EQ(ops.write_sizes, (std::vector<std::size_t>{6, 5, 4, 3, 2, 1}));
}

TEST(FileStorageAppend, AWriteThatTakesNothingIsAFailureNotALoop) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, FakeFileOps::Step{0, 0});
  const auto appended = storage->append(bytes("abc"));
  ASSERT_FALSE(appended.has_value());
  EXPECT_EQ(appended.error(), (JournalError{JournalFault::kStorage, "write made no progress"}));
}

TEST(FileStorageAppend, NothingToAppendWritesNothing) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  EXPECT_TRUE(storage->append({}).has_value());
  EXPECT_TRUE(ops.write_sizes.empty());
}

TEST(FileStorageAppend, RetriesAnInterruptedWrite) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, EINTR, 3);
  ASSERT_TRUE(storage->append(bytes("abc")).has_value());
  EXPECT_EQ(ops.files[kPath], bytes("abc"));
}

TEST(FileStorageAppend, GivesUpOnWritesInterruptedWithoutEnd) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, EINTR, FileStorage::kMaxConsecutiveInterrupts + 1);
  const auto appended = storage->append(bytes("abc"));
  ASSERT_FALSE(appended.has_value());
  expect_storage_fault(appended.error(), EINTR);
  EXPECT_EQ(ops.write_sizes.size(), FileStorage::kMaxConsecutiveInterrupts + 1U);
}

TEST(FileStorageAppend, ToleratesTheMostInterruptsTheBoundAllows) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kWrite, EINTR, FileStorage::kMaxConsecutiveInterrupts);
  EXPECT_TRUE(storage->append(bytes("abc")).has_value());
}

TEST(FileStorageSync, RetriesAnInterruptedFsyncAndSyncsTheJournalDescriptor) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.synced_fds.clear();
  ops.script(Call::kFsync, EINTR, 2);
  ASSERT_TRUE(storage->sync().has_value());
  EXPECT_EQ(ops.synced_fds.size(), 3U);
}

TEST(FileStorageSync, TheDirectoryFsyncIsRetriedToo) {
  FakeFileOps ops;
  ops.script(Call::kFsync, EINTR, 2);
  EXPECT_TRUE(FileStorage::open(ops, kPath).has_value());
}

TEST(FileStorageSync, GivesUpOnFsyncsInterruptedWithoutEnd) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kFsync, EINTR, FileStorage::kMaxConsecutiveInterrupts + 1);
  const auto synced = storage->sync();
  ASSERT_FALSE(synced.has_value());
  expect_storage_fault(synced.error(), EINTR);
}

TEST(FileStorageSync, GivesUpOnTheDirectoryFsyncInterruptedWithoutEnd) {
  FakeFileOps ops;
  ops.script(Call::kFsync, EINTR, FileStorage::kMaxConsecutiveInterrupts + 1);
  const auto storage = FileStorage::open(ops, kPath);
  ASSERT_FALSE(storage.has_value());
  expect_storage_fault(storage.error(), EINTR);
}

TEST(FileStorageRead, GivesUpOnReadsInterruptedWithoutEnd) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kRead, EINTR, FileStorage::kMaxConsecutiveInterrupts + 1);
  const auto read = storage->read_all();
  ASSERT_FALSE(read.has_value());
  expect_storage_fault(read.error(), EINTR);
}

class ReadFailure : public ::testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(Errno, ReadFailure, ::testing::ValuesIn(kReadErrors));

TEST_P(ReadFailure, IsReportedAndClosesTheDescriptor) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kRead, GetParam());
  const auto read = storage->read_all();
  ASSERT_FALSE(read.has_value());
  expect_storage_fault(read.error(), GetParam());
  EXPECT_EQ(ops.open_fds.size(), 1U) << "only the append descriptor remains";
}

TEST_P(ReadFailure, AfterSomeBytesDiscardsThemAndIsReported) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ASSERT_TRUE(storage->append(bytes("abc")).has_value());
  ops.script(Call::kRead, FakeFileOps::Step{0, 1});
  ops.script(Call::kRead, GetParam());
  EXPECT_FALSE(storage->read_all().has_value());
}

TEST_P(OpenFailure, OfTheFileToReadIsReported) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kOpen, GetParam());
  const auto read = storage->read_all();
  ASSERT_FALSE(read.has_value());
  expect_storage_fault(read.error(), GetParam());
}

TEST_P(CloseFailure, OfTheFileReadIsReported) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ASSERT_TRUE(storage->append(bytes("abc")).has_value());
  ops.script(Call::kClose, GetParam());
  const auto read = storage->read_all();
  ASSERT_FALSE(read.has_value());
  expect_storage_fault(read.error(), GetParam());
}

TEST_P(CloseFailure, OfTheFileReadIsNotWhatIsReportedWhenTheReadFailedToo) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ops.script(Call::kRead, EIO);
  ops.script(Call::kClose, GetParam());
  expect_storage_fault(storage->read_all().error(), EIO);
}

TEST(FileStorageRead, AnswersEveryByteInOrderAcrossShortReadsAndInterrupts) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  std::string text;
  for (std::size_t i = 0; i < 2 * FileStorage::kReadChunkBytes + 17; ++i) {
    text.push_back(static_cast<char>('a' + i % 26));
  }
  ASSERT_TRUE(storage->append(bytes(text)).has_value());
  ops.script(Call::kRead, EINTR);
  ops.script(Call::kRead, FakeFileOps::Step{0, 3});
  EXPECT_EQ(storage->read_all().value(), bytes(text));
}

TEST(FileStorageRead, ReadingTwiceAnswersTheSameBytes) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  ASSERT_TRUE(storage->append(bytes("abc")).has_value());
  EXPECT_EQ(storage->read_all().value(), storage->read_all().value());
}

TEST(FileStorageRead, AnEmptyFileIsNoBytes) {
  FakeFileOps ops;
  EXPECT_TRUE(opened(ops)->read_all().value().empty());
}

TEST(FileStorageJournal, AWriterOverFileStorageSurvivesAReplay) {
  FakeFileOps ops;
  const auto storage = opened(ops);
  auto writer = open_writer(*storage);
  ASSERT_TRUE(writer.has_value());
  Entry entry;
  entry.phase = "stress";
  ASSERT_TRUE(writer.value()->append(entry).has_value());
  ASSERT_TRUE(writer.value()->sync().has_value());
  const Replay replay_now = replay(*storage).value();
  EXPECT_EQ(replay_now.entries, std::vector<Entry>{entry});
  EXPECT_EQ(replay_now.ending, Ending::kUnterminated);
  EXPECT_EQ(open_writer(*storage).error().fault, JournalFault::kNotEmpty);
}

}  // namespace
}  // namespace loadforge::journal
