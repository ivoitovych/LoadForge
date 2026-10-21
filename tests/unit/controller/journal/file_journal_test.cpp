// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract's two promises, over a real file instead of memory: a journal
// cut at any byte replays the records before the cut, and a journal with any
// byte changed never replays a changed entry. The suite in
// tests/contracts/journal brings its own memory Storage, so a provider cannot
// hand it a file; these tests are the same scenarios over the file Storage,
// with the file cut and corrupted on disk between the writer and the replay.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "contracts/journal/journal_contract.hpp"
#include "controller/journal/file_storage.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {
namespace {

class FileJournal : public ::testing::Test {
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

  /// A journal of the suite's three entries on disk at `name`, closed or not.
  std::vector<std::byte> write(const std::string& name, bool close) {
    auto storage = FileStorage::open(ops_, path(name));
    EXPECT_TRUE(storage.has_value());
    auto writer = open_writer(*storage.value());
    EXPECT_TRUE(writer.has_value());
    for (const Entry& entry : contracts::journal_suite::entries()) {
      EXPECT_TRUE(writer.value()->append(entry).has_value());
    }
    EXPECT_TRUE(writer.value()->sync().has_value());
    if (close) {
      EXPECT_TRUE(writer.value()->close().has_value());
    }
    return storage.value()->read_all().value();
  }

  /// Replays whatever `bytes` is as the file `name`.
  Replay replay_bytes(const std::string& name, const std::vector<std::byte>& bytes) {
    std::ofstream file(path(name), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    auto storage = FileStorage::open(ops_, path(name));
    EXPECT_TRUE(storage.has_value());
    return replay(*storage.value()).value();
  }

  RealFileOps ops_;
  std::filesystem::path directory_;
};

TEST_F(FileJournal, AClosedJournalReplaysEveryEntryAndEndsCleanly) {
  write("a", true);
  auto storage = FileStorage::open(ops_, path("a"));
  ASSERT_TRUE(storage.has_value());
  const Replay replayed = replay(*storage.value()).value();
  EXPECT_EQ(replayed.entries, contracts::journal_suite::entries());
  EXPECT_EQ(replayed.ending, Ending::kTerminated);
}

TEST_F(FileJournal, AJournalNeverClosedEndedAbnormally) {
  const auto bytes = write("a", false);
  EXPECT_EQ(replay_bytes("b", bytes).ending, Ending::kUnterminated);
}

TEST_F(FileJournal, AFileThatExistsIsNotContinuedByAnotherRun) {
  write("a", false);
  auto storage = FileStorage::open(ops_, path("a"));
  ASSERT_TRUE(storage.has_value());
  EXPECT_EQ(open_writer(*storage.value()).error().fault, JournalFault::kNotEmpty);
}

TEST_F(FileJournal, ACutAtAnyByteKeepsEveryWholeRecordBeforeIt) {
  const auto whole = write("a", true);
  const auto all = contracts::journal_suite::entries();
  std::size_t previous = 0;
  for (std::size_t cut = 0; cut < whole.size(); ++cut) {
    const Replay replayed = replay_bytes(
        "cut",
        std::vector<std::byte>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut)));
    ASSERT_LE(replayed.entries.size(), all.size()) << cut;
    EXPECT_TRUE(std::equal(replayed.entries.begin(), replayed.entries.end(), all.begin())) << cut;
    EXPECT_GE(replayed.entries.size(), previous) << cut;
    previous = replayed.entries.size();
    EXPECT_NE(replayed.ending, Ending::kCorrupt) << cut;
    EXPECT_NE(replayed.ending, Ending::kTerminated) << cut;
  }
  EXPECT_EQ(previous, all.size());
}

TEST_F(FileJournal, AChangedByteIsNeverReadAsAnEntry) {
  const auto whole = write("a", true);
  const auto all = contracts::journal_suite::entries();
  for (std::size_t at = 0; at < whole.size(); ++at) {
    for (const std::byte flip : {std::byte{0x01}, std::byte{0x80}}) {
      std::vector<std::byte> damaged = whole;
      damaged[at] ^= flip;
      const Replay replayed = replay_bytes("damaged", damaged);
      ASSERT_LE(replayed.entries.size(), all.size()) << at;
      EXPECT_TRUE(std::equal(replayed.entries.begin(), replayed.entries.end(), all.begin()))
          << "byte " << at;
      EXPECT_FALSE(replayed.ending == Ending::kTerminated && replayed.entries.size() == all.size())
          << "byte " << at;
    }
  }
}

}  // namespace
}  // namespace loadforge::journal
