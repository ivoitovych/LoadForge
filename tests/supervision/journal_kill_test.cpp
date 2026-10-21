// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7, M5 exit criterion 2: the crash journal survives a hard kill. The shipped
// binary runs a mixed workload with a journal, is killed with SIGKILL in the
// middle of it -- nothing of it runs after that, no handler, no destructor --
// and the journal is replayed by another process. What was synced is there,
// in order, and the replay says the run ended abnormally. The workers it left
// are gone too: none outlives its controller.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "controller/journal/journal.hpp"
#include "main/journal_rig.hpp"

extern char** environ;

namespace loadforge::cli {
namespace {

std::string run_and_capture(const std::vector<std::string>& args) {
  std::string command;
  for (const std::string& each : args) {
    command += "'" + each + "' ";
  }
  command += "2>&1";
  std::string out;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return out;
  }
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    out += buffer.data();
  }
  ::pclose(pipe);
  return out;
}

TEST(JournalHardKill, AControllerKilledMidRunLeavesAJournalThatReplaysAndItsWorkersLeaveWithIt) {
  char pattern[] = "/tmp/loadforge-journal-kill-XXXXXX";
  ASSERT_NE(::mkdtemp(pattern), nullptr);
  const std::string dir = pattern;
  const std::string journal = dir + "/run.lfj";

  const std::vector<std::string> args = {
      LOADFORGE_BINARY, "stress", "--test",     "mixed", "--duration", "120s", "--workers", "2",
      "--footprint",    "4MiB",   "--rotation", "1s",    "--journal",  journal};
  std::vector<char*> argv;
  for (const std::string& each : args) {
    argv.push_back(const_cast<char*>(each.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  ASSERT_EQ(::posix_spawn_file_actions_init(&actions), 0);
  ASSERT_EQ(::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0), 0);
  ASSERT_EQ(::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0), 0);
  posix_spawnattr_t attributes;
  ASSERT_EQ(::posix_spawnattr_init(&attributes), 0);
  ASSERT_EQ(::posix_spawnattr_setpgroup(&attributes, 0), 0);
  ASSERT_EQ(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP), 0);
  pid_t pid = 0;
  ASSERT_EQ(::posix_spawn(&pid, LOADFORGE_BINARY, &actions, &attributes, argv.data(), environ), 0);
  ::posix_spawn_file_actions_destroy(&actions);
  ::posix_spawnattr_destroy(&attributes);

  std::this_thread::sleep_for(std::chrono::milliseconds{4500});
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);

  // Replayed here, from what the kill left on the disk.
  journal::RealFileOps ops;
  RealJournalFiles files;
  const auto replay = read_journal(ops, files, journal);
  ASSERT_TRUE(replay.has_value()) << replay.error();
  EXPECT_TRUE(replay.value().ending == journal::Ending::kUnterminated ||
              replay.value().ending == journal::Ending::kTornTail)
      << journal::describe(replay.value().ending);
  const auto& entries = replay.value().entries;
  ASSERT_GE(entries.size(), 3U) << "an entry at the start and one at each second's rotation";
  for (std::size_t at = 1; at < entries.size(); ++at) {
    EXPECT_GE(entries[at].elapsed_ns, entries[at - 1].elapsed_ns);
  }
  EXPECT_EQ(entries.front().workers_alive, 2U);
  EXPECT_GT(entries.back().elapsed_ns, 2'000'000'000);
  EXPECT_GT(entries.back().last_verified_unit, 0U) << "the workers had verified units by then";

  // And by another process: the shipped binary's own command.
  const std::string said = run_and_capture({LOADFORGE_BINARY, "journal", journal});
  EXPECT_NE(said.find("abnormally"), std::string::npos) << said;

  // None of its workers outlives it: the group the run was spawned in empties.
  bool gone = false;
  for (int waited = 0; waited < 100 && !gone; ++waited) {
    gone = ::kill(-pid, 0) == -1 && errno == ESRCH;
    if (!gone) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
  }
  EXPECT_TRUE(gone) << "the workers were still running ten seconds after the controller died";
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
}

}  // namespace
}  // namespace loadforge::cli
