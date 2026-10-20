// SPDX-License-Identifier: GPL-3.0-or-later
//
// RealFileOps against the kernel (T8 in spirit): every failure here is one the
// kernel really raises, and so is every success. The failures a process
// cannot provoke (ENOSPC, EIO) are forced through FakeFileOps in
// file_storage_test.cpp.

#include "controller/journal/file_ops.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "controller/journal/file_storage.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {
namespace {

class RealFileOpsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "loadforge-journal-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  [[nodiscard]] std::string path(const std::string& name) const {
    return (directory_ / name).string();
  }

  RealFileOps ops_;
  std::filesystem::path directory_;
};

std::vector<std::byte> bytes(const std::string& text) {
  std::vector<std::byte> out;
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

TEST_F(RealFileOpsTest, AppendCreatesWritesSyncsReadsAndCloses) {
  const auto writing = ops_.open(path("a"), FileOps::Mode::kAppend);
  ASSERT_TRUE(writing.has_value());
  const auto text = bytes("hello");
  EXPECT_EQ(ops_.write(writing.value(), text).value(), text.size());
  EXPECT_TRUE(ops_.fsync(writing.value()).has_value());
  EXPECT_TRUE(ops_.close(writing.value()).has_value());

  const auto reading = ops_.open(path("a"), FileOps::Mode::kRead);
  ASSERT_TRUE(reading.has_value());
  std::vector<std::byte> into(16);
  EXPECT_EQ(ops_.read(reading.value(), into).value(), text.size());
  EXPECT_EQ(ops_.read(reading.value(), into).value(), 0U) << "the end of the file";
  EXPECT_TRUE(ops_.close(reading.value()).has_value());
}

TEST_F(RealFileOpsTest, AppendNeverTruncatesAndWritesAtTheEnd) {
  for (const char* text : {"one", "two"}) {
    const auto fd = ops_.open(path("a"), FileOps::Mode::kAppend);
    ASSERT_TRUE(fd.has_value());
    ASSERT_TRUE(ops_.write(fd.value(), bytes(text)).has_value());
    ASSERT_TRUE(ops_.close(fd.value()).has_value());
  }
  EXPECT_EQ(std::filesystem::file_size(path("a")), 6U);
}

TEST_F(RealFileOpsTest, ADirectoryOpensAndSyncs) {
  const auto fd = ops_.open(directory_.string(), FileOps::Mode::kDirectory);
  ASSERT_TRUE(fd.has_value());
  EXPECT_TRUE(ops_.fsync(fd.value()).has_value());
  EXPECT_TRUE(ops_.close(fd.value()).has_value());
}

TEST_F(RealFileOpsTest, TheDescriptorsCloseOnExec) {
  const auto fd = ops_.open(path("a"), FileOps::Mode::kAppend);
  ASSERT_TRUE(fd.has_value());
  EXPECT_NE(::fcntl(fd.value(), F_GETFD) & FD_CLOEXEC, 0);
  EXPECT_TRUE(ops_.close(fd.value()).has_value());
}

TEST_F(RealFileOpsTest, OpenFailuresCarryTheKernelsErrno) {
  EXPECT_EQ(ops_.open(path("no/such/dir/a"), FileOps::Mode::kAppend).error().number, ENOENT);
  EXPECT_EQ(ops_.open(path("missing"), FileOps::Mode::kRead).error().number, ENOENT);
  EXPECT_EQ(ops_.open(directory_.string(), FileOps::Mode::kAppend).error().number, EISDIR);
  const auto file = ops_.open(path("a"), FileOps::Mode::kAppend);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(ops_.open(path("a"), FileOps::Mode::kDirectory).error().number, ENOTDIR);
  EXPECT_TRUE(ops_.close(file.value()).has_value());
}

TEST_F(RealFileOpsTest, ReadWriteFsyncAndCloseFailuresCarryTheKernelsErrno) {
  const auto reading = ops_.open(directory_.string(), FileOps::Mode::kRead);
  ASSERT_TRUE(reading.has_value());
  std::vector<std::byte> into(4);
  EXPECT_EQ(ops_.read(reading.value(), into).error().number, EISDIR);
  EXPECT_EQ(ops_.write(reading.value(), bytes("x")).error().number, EBADF);
  EXPECT_TRUE(ops_.close(reading.value()).has_value());
  EXPECT_EQ(ops_.close(reading.value()).error().number, EBADF);
  EXPECT_EQ(ops_.fsync(reading.value()).error().number, EBADF);
  int pipe_ends[2];
  ASSERT_EQ(::pipe(pipe_ends), 0);
  EXPECT_EQ(ops_.fsync(pipe_ends[1]).error().number, EINVAL);
  ::close(pipe_ends[0]);
  ::close(pipe_ends[1]);
}

TEST_F(RealFileOpsTest, AFileStorageOverTheKernelHoldsAJournal) {
  auto storage = FileStorage::open(ops_, path("run.journal"));
  ASSERT_TRUE(storage.has_value());
  auto writer = open_writer(*storage.value());
  ASSERT_TRUE(writer.has_value());
  Entry entry;
  entry.phase = "warm-up";
  entry.temperature_c = 71.25;
  ASSERT_TRUE(writer.value()->append(entry).has_value());
  ASSERT_TRUE(writer.value()->close().has_value());
  const Replay replayed = replay(*storage.value()).value();
  EXPECT_EQ(replayed.entries, std::vector<Entry>{entry});
  EXPECT_EQ(replayed.ending, Ending::kTerminated);
}

TEST_F(RealFileOpsTest, AFileStorageInAMissingDirectoryIsRefused) {
  const auto storage = FileStorage::open(ops_, path("missing/run.journal"));
  ASSERT_FALSE(storage.has_value());
  EXPECT_EQ(storage.error().fault, JournalFault::kStorage);
}

}  // namespace
}  // namespace loadforge::journal
