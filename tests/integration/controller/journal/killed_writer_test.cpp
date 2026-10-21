// SPDX-License-Identifier: GPL-3.0-or-later
//
// M5 exit criterion 2, the component's half: a writer killed with SIGKILL in
// the middle of appending leaves a journal its parent can replay.
//
// The child appends and syncs entries as fast as it can and tells the parent,
// down a pipe, each one it has synced. The parent kills it after it has heard
// of `confirmed` entries. Whatever the child was doing at that moment -- in
// write, in fsync, between the two -- the journal must hold at least the
// confirmed entries, each exactly as written, in order, and must end
// abnormally. A SIGKILL cannot tear a page-cache write the way a power cut
// can; the torn tails are the contract suite's every-byte cuts, which this
// test does not repeat.
//
// What the child's own execution contributes to coverage is lost with its
// _exit-less death (JOURNAL §1.9), so the same code is covered by the unit
// tests in the parent; this test is about the process boundary.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "controller/journal/file_storage.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {
namespace {

constexpr std::uint64_t kLimit = 1'000'000;

Entry expected(std::uint64_t index) {
  Entry entry;
  entry.elapsed_ns = static_cast<std::int64_t>(index) * 5'000'000'000;
  entry.phase = "stress";
  entry.workload = "compression";
  entry.last_verified_unit = index;
  entry.temperature_c = 60.0 + static_cast<double>(index % 30);
  entry.workers_alive = 4;
  return entry;
}

/// The child: append, sync, tell the parent, until killed. Never returns.
[[noreturn]] void run_writer(const std::string& path, int progress_fd) {
  RealFileOps ops;
  auto storage = FileStorage::open(ops, path);
  if (!storage.has_value()) {
    ::_exit(2);
  }
  auto writer = open_writer(*storage.value());
  if (!writer.has_value()) {
    ::_exit(3);
  }
  for (std::uint64_t index = 0; index < kLimit; ++index) {
    if (!writer.value()->append(expected(index)).has_value() ||
        !writer.value()->sync().has_value()) {
      ::_exit(4);
    }
    const char tick = 1;
    if (::write(progress_fd, &tick, 1) != 1) {
      ::_exit(5);
    }
  }
  ::_exit(6);
}

class KilledWriter : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "loadforge-killed-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  /// Runs a writer on `path`, kills it once `confirmed` entries are synced,
  /// and answers how many the parent had been told of.
  static std::size_t kill_after(const std::string& path, std::size_t confirmed) {
    int pipe_ends[2];
    EXPECT_EQ(::pipe2(pipe_ends, O_CLOEXEC), 0);
    const pid_t child = ::fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
      ::close(pipe_ends[0]);
      run_writer(path, pipe_ends[1]);
    }
    ::close(pipe_ends[1]);
    std::size_t heard = 0;
    char tick = 0;
    while (heard < confirmed && ::read(pipe_ends[0], &tick, 1) == 1) {
      ++heard;
    }
    EXPECT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    EXPECT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status)) << "the child must die of the kill, not of its own accord";
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
    ::close(pipe_ends[0]);
    return heard;
  }

  [[nodiscard]] std::string path(const std::string& name) const {
    return (directory_ / name).string();
  }

  std::filesystem::path directory_;
};

TEST_F(KilledWriter, LeavesEveryConfirmedEntryReplayableAndEndsAbnormally) {
  for (const std::size_t confirmed : {std::size_t{1}, std::size_t{7}, std::size_t{200}}) {
    const std::string file = path("run-" + std::to_string(confirmed));
    const std::size_t heard = kill_after(file, confirmed);
    ASSERT_EQ(heard, confirmed);

    RealFileOps ops;
    auto storage = FileStorage::open(ops, file);
    ASSERT_TRUE(storage.has_value());
    const Replay replayed = replay(*storage.value()).value();
    ASSERT_GE(replayed.entries.size(), confirmed) << "a synced entry is never lost";
    for (std::size_t index = 0; index < replayed.entries.size(); ++index) {
      ASSERT_EQ(replayed.entries[index], expected(index)) << "entry " << index;
    }
    EXPECT_TRUE(replayed.ending == Ending::kUnterminated || replayed.ending == Ending::kTornTail)
        << describe(replayed.ending);
  }
}

TEST_F(KilledWriter, ItsJournalIsNeverContinuedByTheNextRun) {
  const std::string file = path("run");
  ASSERT_EQ(kill_after(file, 3), 3U);
  const std::size_t size_before = std::filesystem::file_size(file);
  RealFileOps ops;
  auto storage = FileStorage::open(ops, file);
  ASSERT_TRUE(storage.has_value());
  EXPECT_EQ(open_writer(*storage.value()).error().fault, JournalFault::kNotEmpty);
  EXPECT_EQ(std::filesystem::file_size(file), size_before) << "opening a journal changes nothing";
}

TEST_F(KilledWriter, ACleanlyClosedJournalFromAnotherProcessEndsCleanly) {
  const std::string file = path("closed");
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    RealFileOps ops;
    auto storage = FileStorage::open(ops, file);
    auto writer = open_writer(*storage.value());
    const bool ok =
        writer.value()->append(expected(0)).has_value() && writer.value()->close().has_value();
    ::_exit(ok ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  RealFileOps ops;
  auto storage = FileStorage::open(ops, file);
  ASSERT_TRUE(storage.has_value());
  const Replay replayed = replay(*storage.value()).value();
  EXPECT_EQ(replayed.entries, std::vector<Entry>{expected(0)});
  EXPECT_EQ(replayed.ending, Ending::kTerminated);
}

}  // namespace
}  // namespace loadforge::journal
