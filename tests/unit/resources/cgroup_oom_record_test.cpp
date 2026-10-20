// SPDX-License-Identifier: GPL-3.0-or-later
//
// The provider's side of the OOM-kill record contract (resources/
// oom_kill_record.hpp): the count is found in this process's memory cgroup,
// read in every state its source can be in, and -- against the real kernel --
// rises for an OOM kill and for nothing else.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Built trees through RealSyscalls, as in cgroup_test.cpp, and one state no
// tree can hold: VANISHED, a count that was read and is gone at the next
// reading, made by reading it and then removing it. Denials go through
// FakeSyscalls' read script, the tests running as root. The real kernel is
// read last: the count of this machine's own cgroup, a SIGKILL that must not
// move it, and -- where a v1 memory cgroup can be made -- a child really
// OOM-killed under a 16 MiB limit, whose death must move it by exactly one.

#include "resources/cgroup_oom_record.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "resources/cgroup.hpp"
#include "support/fake_syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"
#include "unit/resources/machine_tree.hpp"

namespace loadforge::resources {
namespace {

using testing::MachineTree;
using testing::serve;
using topology::Availability;
using topology::DiscoveryFailure;

// --- the count's two files ------------------------------------------------------

TEST(OomCountFileTest, EachVersionKeepsItsOwnFile) {
  EXPECT_EQ(oom_count_file(CgroupVersion::kV2), "memory.events");
  EXPECT_EQ(oom_count_file(CgroupVersion::kV1), "memory.oom_control");
}

TEST(OomCountTest, ReadsTheCountFromEitherFilesShape) {
  // memory.oom_control as the v1 container this was written in writes it,
  // after a child was OOM-killed in it; memory.events as v2 writes it.
  EXPECT_EQ(parse_oom_kill_count({"oom_kill_disable 0", "under_oom 0", "oom_kill 1"}).value_or({}),
            std::optional<std::uint64_t>{1});
  EXPECT_EQ(
      parse_oom_kill_count({"low 0", "high 0", "max 12", "oom 3", "oom_kill 2", "oom_group_kill 0"})
          .value_or({}),
      std::optional<std::uint64_t>{2});
}

TEST(OomCountTest, NoCountLineIsAnOlderKernelNotAMalformedFile) {
  auto none = parse_oom_kill_count({"oom_kill_disable 0", "under_oom 0"});
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_FALSE(none.value().has_value()) << "oom_kill_disable is a different line";
  EXPECT_FALSE(parse_oom_kill_count({}).value_or(1U).has_value());
}

TEST(OomCountTest, EveryMalformationIsRefusedWithItsReason) {
  struct Case {
    std::vector<std::string> lines;
    std::string_view reason;
  };
  const Case cases[] = {
      {{"oom_kill 1", "oom_kill 2"}, "oom_kill appears twice"},
      {{"oom_kill "}, "does not end in a count"},
      {{"oom_kill x"}, "does not end in a count"},
      {{"oom_kill -1"}, "does not end in a count"},
      {{"oom_kill  1"}, "does not end in a count"},
      {{"oom_kill 1 "}, "does not end in a count"},
  };
  for (const Case& c : cases) {
    auto parsed = parse_oom_kill_count(c.lines);
    ASSERT_FALSE(parsed.has_value()) << "accepted " << c.lines.front();
    EXPECT_NE(parsed.error().find(c.reason), std::string::npos) << parsed.error();
  }
}

// --- the record, on built trees ---------------------------------------------------------

class RecordTest : public ::testing::Test {
 protected:
  MachineTree tree{::testing::UnitTest::GetInstance()->current_test_info()->name()};
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  static DiscoveryFailure failure_of(CgroupOomKillRecord& record) {
    auto count = record.read();
    EXPECT_FALSE(count.has_value()) << "read a count it should have refused";
    return count.has_value() ? DiscoveryFailure{} : count.error();
  }
};

TEST_F(RecordTest, AV2CountIsReadFromTheCgroupsMemoryEventsEachTimeItIsAsked) {
  tree.v2("/run.scope");
  tree.write("/sys/fs/cgroup/run.scope/memory.events",
             "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\noom_group_kill 0\n");
  CgroupOomKillRecord record(filesystem, tree.root());
  EXPECT_EQ(record.scope(), tree.at("/sys/fs/cgroup/run.scope/memory.events"));
  EXPECT_EQ(record.kills().value_or(99), 0U);

  tree.write("/sys/fs/cgroup/run.scope/memory.events",
             "low 0\nhigh 0\nmax 4\noom 1\noom_kill 1\noom_group_kill 0\n");
  EXPECT_EQ(record.kills().value_or(99), 1U) << "read again, not remembered";
}

TEST_F(RecordTest, AV1CountIsReadFromTheCgroupsOwnOomControl) {
  tree.v1("/jobs/a1", MachineTree::kSentinel);
  tree.write("/sys/fs/cgroup/memory/jobs/a1/memory.oom_control",
             "oom_kill_disable 0\nunder_oom 0\noom_kill 3\n");
  CgroupOomKillRecord record(filesystem, tree.root());
  EXPECT_EQ(record.scope(), tree.at("/sys/fs/cgroup/memory/jobs/a1/memory.oom_control"));
  EXPECT_EQ(record.read().value_or(99), 3U);
}

TEST_F(RecordTest, NoMemoryCgroupIsAnAbsentCountThatSaysSo) {
  tree.write("/proc/self/cgroup", "1:cpu:/\n");
  CgroupOomKillRecord record(filesystem, tree.root());
  EXPECT_EQ(record.scope(), "the memory cgroup of this process");
  const DiscoveryFailure failure = failure_of(record);
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_EQ(failure.subject, tree.at("/proc/self/cgroup"));
  EXPECT_NE(failure.detail.find("in no memory cgroup"), std::string::npos);

  auto words = record.kills();
  ASSERT_FALSE(words.has_value());
  EXPECT_EQ(words.error(), describe(failure)) << "the contract's words are the fact's";
}

TEST_F(RecordTest, ACgroupThatCannotBeLocatedIsThatFailureAtEveryReading) {
  tree.v2("/a");
  tree.remove("/sys/fs/cgroup/a");
  CgroupOomKillRecord record(filesystem, tree.root());
  EXPECT_EQ(record.scope(), "the memory cgroup of this process");
  EXPECT_EQ(failure_of(record).kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_EQ(failure_of(record).kind, DiscoveryFailure::Kind::kContradiction) << "and again";
}

TEST_F(RecordTest, TheCountFileInEachStateItCanBeIn) {
  tree.v2("/a");
  const std::string events = "/sys/fs/cgroup/a/memory.events";
  CgroupOomKillRecord record(filesystem, tree.root());

  // Absent: a cgroup whose parent does not enable the memory controller.
  DiscoveryFailure failure = failure_of(record);
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_EQ(failure.subject, tree.at(events));

  // Unreadable: our own path is wrong, not the kernel short of a count.
  tree.make_directory_of(events);
  EXPECT_EQ(failure_of(record).availability, Availability::kUnreadable);
  tree.remove(events);

  // Present without the line: a kernel before 4.13.
  tree.write(events, "low 0\nhigh 0\nmax 0\noom 0\n");
  failure = failure_of(record);
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_NE(failure.detail.find("4.13"), std::string::npos);

  tree.write(events, "oom_kill many\n");
  failure = failure_of(record);
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.subject, tree.at(events));

  // Vanished: it was read, and it is gone. Only a record that remembers
  // reading it can say so; a fresh one would call the same ENOENT absent.
  tree.write(events, "oom_kill 0\n");
  ASSERT_EQ(record.read().value_or(99), 0U);
  tree.remove(events);
  EXPECT_EQ(failure_of(record).availability, Availability::kVanished);
  CgroupOomKillRecord fresh(filesystem, tree.root());
  EXPECT_EQ(failure_of(fresh).availability, Availability::kAbsent);
}

TEST(RecordDeniedTest, ADeniedCountIsDeniedAndNamed) {
  using testing::kServedControllers;
  using testing::kServedMembership;
  using testing::kServedMountinfo;
  loadforge::testing::FakeSyscalls syscalls;
  serve(syscalls, {{kServedMembership},
                   {kServedMountinfo},
                   {kServedControllers},
                   {"", EACCES},
                   {"oom_kill 7\n"}});
  platform::FileSystem filesystem(syscalls);
  CgroupOomKillRecord record(filesystem, "");
  auto denied = record.read();
  ASSERT_FALSE(denied.has_value());
  EXPECT_EQ(denied.error().availability, Availability::kDenied);
  EXPECT_EQ(denied.error().subject, "/sys/fs/cgroup/run.scope/memory.events");
  EXPECT_EQ(record.read().value_or(99), 7U) << "and readable once permitted";
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

// --- the real kernel (T8) ----------------------------------------------------------------

TEST(RecordRealTest, ThisMachinesCountIsReadAndASigkillThatIsNotAnOomKillDoesNotMoveIt) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  CgroupOomKillRecord record(filesystem, kMachineRoot);
  auto before = record.read();
  if (!before.has_value()) {
    // A machine without a memory cgroup, or with one that keeps no count, is
    // a machine on which OOM kills stay kills; it must still say why.
    ASSERT_EQ(before.error().availability, Availability::kAbsent) << describe(before.error());
    GTEST_SKIP() << "no OOM-kill count on this machine: " << describe(before.error());
  }

  // A child of this process, in this cgroup, killed with SIGKILL by this
  // process: the OOM killer's signal from somebody else. The count must not
  // move -- if it did, every kill -9 would read as an OOM kill.
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::pause();
    ::_exit(0);
  }
  ASSERT_EQ(::kill(child, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  auto after = record.read();
  ASSERT_TRUE(after.has_value()) << describe(after.error());
  EXPECT_EQ(after.value(), before.value());
}

/// Writes one cgroup control file as root does: a failure is reported, not
/// thrown, because a host that refuses is a reason to skip, not to fail.
bool control(const std::string& path, const std::string& value) {
  std::ofstream out(path);
  out << value;
  out.flush();
  return out.good();
}

TEST(RecordRealTest, AChildReallyOomKilledInAV1CgroupMovesThatCgroupsCountByOne) {
  // The kernel's record against the kernel's act. A v1 memory cgroup is made
  // under this process's own, limited to 16 MiB with swapping off; a child is
  // moved into it before it allocates, and allocates until the OOM killer
  // takes it. Its status must be SIGKILL and the cgroup's count must rise by
  // exactly one -- read through this module's own parser and file name.
  //
  // It needs a v1 memory cgroup this process may write, which means root on a
  // v1 or hybrid host. On v2 the leaf this process lives in cannot be given a
  // child with the memory controller enabled while it holds processes, and an
  // unprivileged runner cannot make one at all; there the test says so and
  // skips. The classification this corroborates is driven by the controller's
  // tests on every host.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto located = locate_memory_cgroup(filesystem, kMachineRoot);
  ASSERT_TRUE(located.has_value()) << describe(located.error());
  if (!located.value().has_value() || located.value()->version != CgroupVersion::kV1) {
    GTEST_SKIP() << "needs a v1 memory cgroup; this machine's memory controller is elsewhere";
  }
  const std::string cgroup =
      directory_of(*located.value()) + "/loadforge-oom-" + std::to_string(::getpid());
  std::error_code made;
  if (!std::filesystem::create_directory(cgroup, made) ||
      !control(cgroup + "/memory.limit_in_bytes", "16777216") ||
      !control(cgroup + "/memory.swappiness", "0")) {
    std::filesystem::remove(cgroup, made);
    GTEST_SKIP() << "cannot make a limited memory cgroup at " << cgroup;
  }
  const std::string count_file = cgroup + "/" + std::string{oom_count_file(CgroupVersion::kV1)};
  topology::Source count(filesystem, count_file);
  const auto read_count = [&count]() -> std::optional<std::uint64_t> {
    auto lines = count.lines();
    if (!lines.has_value()) {
      return std::nullopt;
    }
    return parse_oom_kill_count(lines.value()).value_or(std::nullopt);
  };
  const std::optional<std::uint64_t> before = read_count();

  int go[2] = {-1, -1};
  ASSERT_EQ(::pipe(go), 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    char byte = 0;
    ::close(go[1]);
    if (::read(go[0], &byte, 1) != 1) {
      ::_exit(2);
    }
    // Four times the limit, every page touched so it is really charged.
    constexpr std::size_t kBytes = std::size_t{64} << 20U;
    std::vector<char> backing(kBytes);
    volatile char* memory = backing.data();
    for (std::size_t i = 0; i < kBytes; i += 4096) {
      memory[i] = 1;
    }
    ::_exit(3);  // Reached only if the limit did not hold.
  }
  ::close(go[0]);
  const bool moved = control(cgroup + "/cgroup.procs", std::to_string(child));
  const char byte = 'g';
  EXPECT_EQ(::write(go[1], &byte, 1), 1);
  ::close(go[1]);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  const std::optional<std::uint64_t> after = read_count();
  for (int attempt = 0; attempt < 100 && !std::filesystem::remove(cgroup, made); ++attempt) {
    ::usleep(10'000);  // The cgroup empties as the dead child is released.
  }

  ASSERT_TRUE(moved) << "could not move the child into " << cgroup;
  ASSERT_TRUE(WIFSIGNALED(status)) << "status " << status;
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
  ASSERT_TRUE(before.has_value() && after.has_value()) << count_file;
  EXPECT_EQ(*after, *before + 1);
}

}  // namespace
}  // namespace loadforge::resources
