// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding this process's memory cgroup: which hierarchy, which path, where it
// is mounted, and whether the directory is really there.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The trees are built here, on disk, per test, and read through RealSyscalls:
// /proc/self/cgroup, /proc/self/mountinfo and the cgroup directories, in each
// shape the kernel writes and each state a source can be in. A committed tree
// cannot hold the states that matter most -- a denied file arrives from a
// clone readable, and the tests run as root besides -- so the denials are
// aimed at one source at a time through FakeSyscalls' read script instead.
// An absent file is a file not written; a malformed one is a line the kernel
// would not write; an unreadable one is a directory where the file should be
// (a real EISDIR). The real machine is read last (T8), and the directory found
// there is checked against the kernel's own cgroup.procs: the pid of this test
// must be in it, which no tree written here could arrange.

#include "resources/cgroup.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "resources/decimal.hpp"
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

// --- parse_decimal -----------------------------------------------------------

TEST(DecimalTest, DigitsAndNothingElseUpToTheLargestUnsigned64BitValue) {
  EXPECT_EQ(parse_decimal("0"), 0U);
  EXPECT_EQ(parse_decimal("14343688192"), 14343688192U);
  EXPECT_EQ(parse_decimal("18446744073709551615"), 18446744073709551615U) << "exactly the bound";
  EXPECT_FALSE(parse_decimal("18446744073709551616").has_value()) << "one past it";
  EXPECT_FALSE(parse_decimal("99999999999999999999999").has_value());
  for (const std::string_view text : {"", " 1", "1 ", "+1", "-1", "0x10", "1a", "a1", "1.0"}) {
    EXPECT_FALSE(parse_decimal(text).has_value()) << "accepted \"" << text << "\"";
  }
}

// --- describe ----------------------------------------------------------------

TEST(CgroupVersionTest, EachVersionHasItsWords) {
  EXPECT_EQ(describe(CgroupVersion::kV1), "cgroup v1");
  EXPECT_EQ(describe(CgroupVersion::kV2), "cgroup v2");
  EXPECT_EQ(describe(static_cast<CgroupVersion>(9)), "unrecognised cgroup version");
}

// --- parse_membership ----------------------------------------------------------

std::optional<CgroupMembership> membership(const std::vector<std::string>& lines) {
  auto parsed = parse_membership(lines);
  EXPECT_TRUE(parsed.has_value()) << parsed.error();
  return parsed.value_or(std::nullopt);
}

TEST(MembershipTest, AV1MemoryHierarchyWinsOverTheUnifiedLine) {
  // The hybrid layout read off the container this was written in.
  const auto found = membership(
      {"9:name=systemd:/", "8:pids:/", "4:memory:/jobs/a1/shell", "3:cpuset:/", "1:cpu:/", "0::/"});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->version, CgroupVersion::kV1);
  EXPECT_EQ(found->path, "/jobs/a1/shell");
}

TEST(MembershipTest, TheUnifiedLineAloneIsV2) {
  const auto found = membership({"0::/user.slice/user-1000.slice/session-2.scope"});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->version, CgroupVersion::kV2);
  EXPECT_EQ(found->path, "/user.slice/user-1000.slice/session-2.scope");
}

TEST(MembershipTest, MemoryCoMountedWithOtherControllersIsStillTheMemoryHierarchy) {
  for (const char* line : {"3:memory,hugetlb:/x", "3:hugetlb,memory:/x", "3:a,memory,b:/x"}) {
    const auto found = membership({line, "0::/"});
    ASSERT_TRUE(found.has_value()) << line;
    EXPECT_EQ(found->version, CgroupVersion::kV1) << line;
    EXPECT_EQ(found->path, "/x") << line;
  }
}

TEST(MembershipTest, AControllerWhoseNameOnlyContainsMemoryIsNotIt) {
  const auto found = membership({"5:memory_tiering:/a", "6:xmemory:/b", "0::/c"});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->version, CgroupVersion::kV2);
  EXPECT_EQ(found->path, "/c");
}

TEST(MembershipTest, NoMemoryHierarchyAndNoUnifiedLineIsNoMemoryCgroup) {
  EXPECT_FALSE(membership({"1:cpu:/", "2:name=systemd:/"}).has_value());
  EXPECT_FALSE(membership({}).has_value()) << "no hierarchy at all";
}

TEST(MembershipTest, APathMayItselfContainAColon) {
  const auto found = membership({"0::/odd:name"});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->path, "/odd:name");
}

TEST(MembershipTest, EveryLineTheKernelWouldNotWriteIsRefusedWithItsReason) {
  struct Case {
    std::vector<std::string> lines;
    std::string_view reason;
  };
  const Case cases[] = {
      {{"garbage"}, "is not hierarchy-id:controllers:path"},
      {{"4:memory"}, "is not hierarchy-id:controllers:path"},
      {{""}, "is not hierarchy-id:controllers:path"},
      {{"x:memory:/a"}, "does not begin with a hierarchy id"},
      {{":memory:/a"}, "does not begin with a hierarchy id"},
      {{"4:memory:relative"}, "does not end in an absolute cgroup path"},
      {{"4:memory:"}, "does not end in an absolute cgroup path"},
      {{"0:memory:/a"}, "lists controllers on the unified hierarchy"},
      {{"0::/a", "0::/b"}, "the unified hierarchy is listed twice"},
      {{"4:memory:/a", "5:memory:/b"}, "two hierarchies both claim the memory controller"},
  };
  for (const Case& c : cases) {
    auto parsed = parse_membership(c.lines);
    ASSERT_FALSE(parsed.has_value()) << "accepted " << c.lines.front();
    EXPECT_NE(parsed.error().find(c.reason), std::string::npos)
        << c.lines.front() << " -> " << parsed.error();
  }
}

// --- parse_mount_entry ----------------------------------------------------------

TEST(MountEntryTest, ReadsTheLinesTheKernelWrites) {
  auto v1 = parse_mount_entry(
      "41 37 0:33 / /sys/fs/cgroup/memory rw,relatime - cgroup cgroup "
      "rw,memory");
  ASSERT_TRUE(v1.has_value()) << v1.error();
  EXPECT_EQ(v1.value().root, "/");
  EXPECT_EQ(v1.value().mount_point, "/sys/fs/cgroup/memory");
  EXPECT_EQ(v1.value().fstype, "cgroup");
  EXPECT_EQ(v1.value().super_options, "rw,memory");

  // One optional field, then two: the separator is found wherever it falls.
  auto v2 = parse_mount_entry(
      "35 24 0:30 / /sys/fs/cgroup rw,nosuid shared:9 - cgroup2 cgroup2 rw,nsdelegate");
  ASSERT_TRUE(v2.has_value()) << v2.error();
  EXPECT_EQ(v2.value().fstype, "cgroup2");
  EXPECT_EQ(v2.value().super_options, "rw,nsdelegate");
  auto container = parse_mount_entry(
      "1234 1230 0:33 /docker/ab12 /sys/fs/cgroup/memory ro,nosuid "
      "shared:4 master:15 - cgroup cgroup rw,memory");
  ASSERT_TRUE(container.has_value()) << container.error();
  EXPECT_EQ(container.value().root, "/docker/ab12");
}

TEST(MountEntryTest, PathsAreUnescapedTheWayTheKernelEscapedThem) {
  auto entry =
      parse_mount_entry("1 2 0:3 /a\\040b\\011c /mnt/x\\134y\\012z rw - cgroup2 cgroup2 rw");
  ASSERT_TRUE(entry.has_value()) << entry.error();
  EXPECT_EQ(entry.value().root, "/a b\tc");
  EXPECT_EQ(entry.value().mount_point, "/mnt/x\\y\nz");
}

TEST(MountEntryTest, EveryShapeTheKernelDoesNotWriteIsRefusedWithItsReason) {
  struct Case {
    std::string_view line;
    std::string_view reason;
  };
  const Case cases[] = {
      {"", "single spaces"},
      {"1 2 0:3 / /m rw  - cgroup2 cgroup2 rw", "single spaces"},
      {" 1 2 0:3 / /m rw - cgroup2 cgroup2 rw", "single spaces"},
      {"1 2 0:3 / /m rw - cgroup2 cgroup2 rw ", "single spaces"},
      {"1 2 0:3 / /m rw cgroup2 cgroup2 rw", "no \"-\""},
      {"1 2 0:3 / /m - cgroup2 cgroup2 rw", "no \"-\""},
      {"1 2 0:3 / /m rw - cgroup2 cgroup2", "no \"-\""},
      {"1 2 0:3 / /m rw - cgroup2 cgroup2 rw extra", "no \"-\""},
      {"1 2 0:3 /a\\04 /m rw - cgroup2 cgroup2 rw", "escape"},
      {"1 2 0:3 / /m\\0 rw - cgroup2 cgroup2 rw", "escape"},
      {"1 2 0:3 / /m\\ rw - cgroup2 cgroup2 rw", "escape"},
      {"1 2 0:3 /a\\08x /m rw - cgroup2 cgroup2 rw", "escape"},
      {"1 2 0:3 /a\\/12 /m rw - cgroup2 cgroup2 rw", "escape"},
      {"1 2 0:3 / /m\\777 rw - cgroup2 cgroup2 rw", "escape"},
  };
  for (const Case& c : cases) {
    auto entry = parse_mount_entry(c.line);
    ASSERT_FALSE(entry.has_value()) << "accepted \"" << c.line << "\"";
    EXPECT_NE(entry.error().find(c.reason), std::string::npos) << c.line << " -> " << entry.error();
  }
}

// --- directories ----------------------------------------------------------------

TEST(DirectoriesTest, FromTheCgroupUpToItsMountAndNoFurther) {
  const MemoryCgroup leaf{CgroupVersion::kV2, "/r/sys/fs/cgroup", "/a/b"};
  EXPECT_EQ(directory_of(leaf), "/r/sys/fs/cgroup/a/b");
  EXPECT_EQ(
      directories_upward(leaf),
      (std::vector<std::string>{"/r/sys/fs/cgroup/a/b", "/r/sys/fs/cgroup/a", "/r/sys/fs/cgroup"}));
  const MemoryCgroup mount_itself{CgroupVersion::kV1, "/sys/fs/cgroup/memory", ""};
  EXPECT_EQ(directory_of(mount_itself), "/sys/fs/cgroup/memory");
  EXPECT_EQ(directories_upward(mount_itself), (std::vector<std::string>{"/sys/fs/cgroup/memory"}));
}

// --- locate_memory_cgroup, on built trees ------------------------------------------

class LocateTest : public ::testing::Test {
 protected:
  MachineTree tree{::testing::UnitTest::GetInstance()->current_test_info()->name()};
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  MemoryCgroup located() {
    auto result = locate_memory_cgroup(filesystem, tree.root());
    EXPECT_TRUE(result.has_value()) << describe(result.error());
    EXPECT_TRUE(result.has_value() && result.value().has_value()) << "no memory cgroup found";
    return result.value_or(std::nullopt).value_or(MemoryCgroup{});
  }
  DiscoveryFailure refused() {
    auto result = locate_memory_cgroup(filesystem, tree.root());
    EXPECT_FALSE(result.has_value()) << "located a cgroup it should have refused";
    return result.has_value() ? DiscoveryFailure{} : result.error();
  }
};

TEST_F(LocateTest, TheV2HostLayout) {
  tree.v2("/user.slice/run.scope");
  const MemoryCgroup cgroup = located();
  EXPECT_EQ(cgroup.version, CgroupVersion::kV2);
  EXPECT_EQ(cgroup.mount, tree.at("/sys/fs/cgroup"));
  EXPECT_EQ(cgroup.relative, "/user.slice/run.scope");
}

TEST_F(LocateTest, TheV2RootItselfAsAContainerWithACgroupNamespaceSeesIt) {
  tree.v2("/");
  const MemoryCgroup cgroup = located();
  EXPECT_EQ(cgroup.relative, "");
  EXPECT_EQ(directory_of(cgroup), tree.at("/sys/fs/cgroup"));
}

TEST_F(LocateTest, TheHybridLayoutUsesTheV1MemoryHierarchyNotTheUnifiedOne) {
  tree.v1("/jobs/a1/shell", MachineTree::kSentinel);
  const MemoryCgroup cgroup = located();
  EXPECT_EQ(cgroup.version, CgroupVersion::kV1);
  EXPECT_EQ(cgroup.mount, tree.at("/sys/fs/cgroup/memory"));
  EXPECT_EQ(cgroup.relative, "/jobs/a1/shell");
}

TEST_F(LocateTest, AV1ContainerWithoutACgroupNamespaceIsFoundAtItsMountNotBelowIt) {
  // The case the mountinfo read exists for. /proc/self/cgroup names the
  // container's cgroup by its full host path, and the runtime mounts exactly
  // that directory at /sys/fs/cgroup/memory: joining the two would look for
  // /sys/fs/cgroup/memory/docker/ab12, which does not exist.
  tree.write("/proc/self/cgroup", "4:memory:/docker/ab12\n0::/\n");
  tree.write("/proc/self/mountinfo",
             "1234 1230 0:33 /docker/ab12 /sys/fs/cgroup/memory ro,nosuid master:15 - cgroup "
             "cgroup rw,memory\n");
  tree.v1_directory("", "1073741824");
  const MemoryCgroup cgroup = located();
  EXPECT_EQ(cgroup.relative, "");
  EXPECT_EQ(directory_of(cgroup), tree.at("/sys/fs/cgroup/memory"));
  EXPECT_FALSE(std::filesystem::exists(tree.at("/sys/fs/cgroup/memory/docker/ab12")));
}

TEST_F(LocateTest, AMountRootCoversItsSubtreeByWholeComponents) {
  tree.write("/proc/self/cgroup", "4:memory:/docker/ab12/inner\n");
  tree.write("/proc/self/mountinfo",
             "1 2 0:33 /docker/ab12 /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n");
  tree.v1_directory("/inner", "1073741824");
  EXPECT_EQ(located().relative, "/inner");

  // "/docker/ab" is a prefix of "/docker/ab12" as text and not as a path.
  tree.write("/proc/self/mountinfo",
             "1 2 0:33 /docker/ab /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n");
  const DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(failure.detail.find("no mount of that hierarchy shows it"), std::string::npos)
      << failure.detail;
}

TEST_F(LocateTest, TheFirstMountThatShowsTheCgroupIsUsed) {
  // A bind mount of another subtree comes first and does not cover the
  // cgroup; a second v1 hierarchy of a different controller is not memory's.
  tree.write("/proc/self/cgroup", "4:memory:/a\n1:cpu:/\n0::/\n");
  tree.write("/proc/self/mountinfo",
             "38 37 0:30 / /sys/fs/cgroup/cpu rw - cgroup cgroup rw,cpu\n"
             "40 37 0:33 /b /mnt/elsewhere rw - cgroup cgroup rw,memory\n"
             "41 37 0:33 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n"
             "42 37 0:33 / /mnt/second rw - cgroup cgroup rw,memory\n");
  tree.v1_directory("/a", "1073741824");
  const MemoryCgroup cgroup = located();
  EXPECT_EQ(cgroup.mount, tree.at("/sys/fs/cgroup/memory"));
  EXPECT_EQ(cgroup.relative, "/a");
}

TEST_F(LocateTest, AMountPointWithASpaceIsOpenedUnescaped) {
  tree.write("/proc/self/cgroup", "0::/a\n");
  tree.write("/proc/self/mountinfo",
             "35 24 0:30 / /sys/fs/my\\040cgroup rw - cgroup2 cgroup2 rw\n");
  tree.write("/sys/fs/my cgroup/a/cgroup.controllers", "memory\n");
  EXPECT_EQ(located().mount, tree.at("/sys/fs/my cgroup"));
}

TEST_F(LocateTest, NoCgroupFileIsAKernelWithoutCgroupsAndNoMemoryCgroup) {
  tree.directory("/proc/self");
  auto result = locate_memory_cgroup(filesystem, tree.root());
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().has_value());
}

TEST_F(LocateTest, NoMemoryHierarchyIsNoMemoryCgroupAndMountinfoIsNotNeeded) {
  tree.write("/proc/self/cgroup", "1:cpu:/\n");
  auto result = locate_memory_cgroup(filesystem, tree.root());
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().has_value());
}

TEST_F(LocateTest, AMalformedOrUnreadableMembershipIsAFailureNamingTheFile) {
  tree.write("/proc/self/cgroup", "garbage\n");
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.subject, tree.at("/proc/self/cgroup"));
  EXPECT_NE(failure.detail.find("hierarchy-id:controllers:path"), std::string::npos);

  tree.make_directory_of("/proc/self/cgroup");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kUnreadable);
  EXPECT_EQ(failure.subject, tree.at("/proc/self/cgroup"));
}

TEST_F(LocateTest, MountinfoAbsentMalformedOrUnreadableIsAFailureNotNoLimit) {
  tree.v2("/a");
  tree.remove("/proc/self/mountinfo");
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_EQ(failure.subject, tree.at("/proc/self/mountinfo"));

  tree.write("/proc/self/mountinfo", "35 24 0:30 / /sys/fs/cgroup rw cgroup2 cgroup2 rw\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_NE(failure.detail.find("/sys/fs/cgroup rw cgroup2"), std::string::npos)
      << "names the line: " << failure.detail;

  tree.make_directory_of("/proc/self/mountinfo");
  EXPECT_EQ(refused().availability, Availability::kUnreadable);
}

TEST_F(LocateTest, AHierarchyNotMountedWhereThisProcessCanSeeItIsAContradiction) {
  tree.write("/proc/self/cgroup", "4:memory:/a\n0::/\n");
  // Only the unified hierarchy is mounted; memory's v1 one is not.
  tree.write("/proc/self/mountinfo",
             "47 37 0:39 / /sys/fs/cgroup/unified rw - cgroup2 cgroup2 rw\n");
  const DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_EQ(failure.subject, tree.at("/proc/self/mountinfo"));
  EXPECT_NE(failure.detail.find("cgroup v1 memory hierarchy"), std::string::npos) << failure.detail;
  EXPECT_NE(failure.detail.find("not mounted where this process can see it"), std::string::npos);
}

TEST_F(LocateTest, ADirectoryThatIsNotThereIsAContradictionNotAnAbsentLimit) {
  tree.v2("/a");
  tree.remove("/sys/fs/cgroup/a");
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/a/cgroup.controllers"));
  EXPECT_NE(failure.detail.find("it is not there"), std::string::npos);

  tree.v1("/a", MachineTree::kSentinel);
  tree.remove("/sys/fs/cgroup/memory/a");
  failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/memory/a/memory.limit_in_bytes"));
}

TEST_F(LocateTest, AProbeThatCannotBeReadIsAFailureOfItsOwnKind) {
  tree.v2("/a");
  tree.make_directory_of("/sys/fs/cgroup/a/cgroup.controllers");
  const DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(failure.availability, Availability::kUnreadable);
}

// --- denials, aimed through the fake ------------------------------------------------

TEST(LocateDeniedTest, EachOfTheThreeSourcesDeniedIsDeniedAndNamed) {
  using testing::kServedControllers;
  using testing::kServedMembership;
  using testing::kServedMountinfo;
  struct Case {
    std::vector<testing::Served> files;
    std::string subject;
  };
  const Case cases[] = {
      {{{"", EACCES}}, "/proc/self/cgroup"},
      {{{kServedMembership}, {"", EACCES}}, "/proc/self/mountinfo"},
      {{{kServedMembership}, {kServedMountinfo}, {"", EPERM}},
       "/sys/fs/cgroup/run.scope/cgroup.controllers"},
  };
  for (const Case& c : cases) {
    loadforge::testing::FakeSyscalls syscalls;
    serve(syscalls, c.files);
    platform::FileSystem filesystem(syscalls);
    auto result = locate_memory_cgroup(filesystem, "");
    ASSERT_FALSE(result.has_value()) << c.subject;
    EXPECT_EQ(result.error().kind, DiscoveryFailure::Kind::kSource);
    EXPECT_EQ(result.error().availability, Availability::kDenied) << c.subject;
    EXPECT_EQ(result.error().subject, c.subject);
    EXPECT_TRUE(syscalls.all_descriptors_closed());
  }

  // And all three served, the cgroup is found where the fake said.
  loadforge::testing::FakeSyscalls syscalls;
  serve(syscalls, {{kServedMembership}, {kServedMountinfo}, {kServedControllers}});
  platform::FileSystem filesystem(syscalls);
  auto result = locate_memory_cgroup(filesystem, "");
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().has_value());
  EXPECT_EQ(directory_of(*result.value()), "/sys/fs/cgroup/run.scope");
}

// --- the real machine (T8) ----------------------------------------------------------

TEST(LocateRealTest, ThisMachinesMemoryCgroupIsFoundAndHoldsThisProcess) {
  // A tree these tests wrote is a tree these tests believe in (F21). The real
  // one is read here, and the directory found must list this process in the
  // kernel's own cgroup.procs -- a second source, written by the kernel from
  // the task's css_set, that no mistake in this reader can make agree.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto result = locate_memory_cgroup(filesystem, kMachineRoot);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  if (!result.value().has_value()) {
    GTEST_SKIP() << "this machine has no memory cgroup; nothing to cross-check";
  }
  const std::string directory = directory_of(*result.value());
  topology::Source procs(filesystem, directory + "/cgroup.procs");
  auto pids = procs.lines();
  ASSERT_TRUE(pids.has_value()) << describe(pids.error());
  bool listed = false;
  for (const std::string& pid : pids.value()) {
    listed = listed || pid == std::to_string(::getpid());
  }
  EXPECT_TRUE(listed) << "pid " << ::getpid() << " is not in " << directory << "/cgroup.procs";
}

}  // namespace
}  // namespace loadforge::resources
