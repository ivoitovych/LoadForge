// SPDX-License-Identifier: GPL-3.0-or-later
//
// The memory budget (docs/PLAN.md F7): MemAvailable, and the limit that binds
// this process's memory cgroup under either hierarchy.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Built trees, read through RealSyscalls, as in cgroup_test.cpp and for the
// same reasons: each source -- /proc/meminfo, v1's memory.stat, every v2
// memory.max on the way up -- is put in each state a source can be in, and
// the denials are aimed one source at a time through FakeSyscalls' read
// script, because the tests run as root and a denied file cannot survive a
// clone. The real machine is read last (T8) and its v1 figure is checked
// against the per-level limits the kernel publishes beside it.

#include "resources/memory_budget.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
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

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;

// --- parse_mem_available -------------------------------------------------------

TEST(MemAvailableTest, ReadsTheLineTheKernelWritesInBytes) {
  // The lines read off the machine this was written on, padding and all.
  const std::vector<std::string> lines = {"MemTotal:       16479424 kB",
                                          "MemFree:         6702632 kB",
                                          "MemAvailable:   15632336 kB", "Buffers:  361524 kB"};
  auto parsed = parse_mem_available(lines);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value(), std::optional<std::uint64_t>{15632336 * kKiB});
  EXPECT_EQ(parse_mem_available({"MemAvailable:0 kB"}).value_or(std::nullopt), 0U)
      << "no padding at all is still the format";
}

TEST(MemAvailableTest, NoLineIsAKernelThatDoesNotOfferItNotAMalformedFile) {
  auto none = parse_mem_available({"MemTotal:       16479424 kB", "MemFree:  1 kB"});
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_FALSE(none.value().has_value());
  EXPECT_FALSE(parse_mem_available({}).value_or(1U).has_value());
  EXPECT_FALSE(parse_mem_available({"MemAvailableX:  1 kB"}).value_or(1U).has_value())
      << "a different key that begins the same way";
}

TEST(MemAvailableTest, TheLargestByteCountIsAcceptedAndOneKilobyteMoreIsRefused) {
  const std::uint64_t largest_kb = UINT64_MAX / 1024;
  EXPECT_EQ(
      parse_mem_available({"MemAvailable: " + std::to_string(largest_kb) + " kB"}).value_or({}),
      std::optional<std::uint64_t>{largest_kb * 1024});
  EXPECT_FALSE(
      parse_mem_available({"MemAvailable: " + std::to_string(largest_kb + 1) + " kB"}).has_value());
}

TEST(MemAvailableTest, EveryMalformationIsRefusedWithItsReason) {
  struct Case {
    std::vector<std::string> lines;
    std::string_view reason;
  };
  const Case cases[] = {
      {{"MemAvailable: 1 kB", "MemAvailable: 2 kB"}, "appears twice"},
      {{"MemAvailable: 15632336"}, "is not in kB"},
      {{"MemAvailable: 15632336 MB"}, "is not in kB"},
      {{"MemAvailable: 15632336 kB extra"}, "is not in kB"},
      {{"MemAvailable: kB"}, "is not in kB"},
      {{"MemAvailable:"}, "is not in kB"},
      {{"MemAvailable: -5 kB"}, "does not hold a number"},
      {{"MemAvailable: 1 2 kB"}, "does not hold a number"},
      {{"MemAvailable:\t5 kB"}, "does not hold a number"},
  };
  for (const Case& c : cases) {
    auto parsed = parse_mem_available(c.lines);
    ASSERT_FALSE(parsed.has_value()) << "accepted " << c.lines.front();
    EXPECT_NE(parsed.error().find(c.reason), std::string::npos)
        << c.lines.front() << " -> " << parsed.error();
  }
}

// --- parse_memory_limit and the v1 sentinel -------------------------------------------

std::optional<std::uint64_t> limit(std::string_view text, CgroupVersion version) {
  auto parsed = parse_memory_limit(text, version);
  EXPECT_TRUE(parsed.has_value()) << parsed.error();
  return parsed.value_or(std::nullopt);
}

TEST(MemoryLimitTest, V2SaysMaxForNoLimitAndBytesOtherwise) {
  EXPECT_FALSE(limit("max", CgroupVersion::kV2).has_value());
  EXPECT_EQ(limit("1073741824", CgroupVersion::kV2), std::optional<std::uint64_t>{kGiB});
  EXPECT_EQ(limit("0", CgroupVersion::kV2), std::optional<std::uint64_t>{0})
      << "a limit of nothing is a limit, and every footprint is then refused";
  EXPECT_EQ(limit("9223372036854775807", CgroupVersion::kV2),
            std::optional<std::uint64_t>{kLargestLimit});
}

TEST(MemoryLimitTest, V1SaysTheSentinelForNoLimitOnEveryPageSize) {
  // LONG_MAX rounded down to a 4, 16, 64 and 256 KiB page: the first read off
  // a real v1 cgroup after writing -1, the others computed the way the kernel
  // computes them.
  for (const std::string_view sentinel : {"9223372036854771712", "9223372036854759424",
                                          "9223372036854710272", "9223372036854513664"}) {
    EXPECT_FALSE(limit(sentinel, CgroupVersion::kV1).has_value()) << sentinel;
  }
  EXPECT_EQ(limit("14343688192", CgroupVersion::kV1), std::optional<std::uint64_t>{14343688192})
      << "the limit of the container this was written in";
  EXPECT_EQ(limit(std::to_string(kV1UnlimitedFloor - 1), CgroupVersion::kV1),
            std::optional<std::uint64_t>{kV1UnlimitedFloor - 1})
      << "one below the floor is a limit";
  EXPECT_FALSE(limit(std::to_string(kV1UnlimitedFloor), CgroupVersion::kV1).has_value())
      << "the floor itself is the sentinel";
}

TEST(MemoryLimitTest, WhatNeitherVersionWritesIsRefusedNamingTheVersion) {
  struct Case {
    std::string_view text;
    CgroupVersion version;
  };
  const Case cases[] = {
      {"max", CgroupVersion::kV1},
      {"", CgroupVersion::kV2},
      {" 5", CgroupVersion::kV2},
      {"-1", CgroupVersion::kV1},
      {"5G", CgroupVersion::kV2},
      {"9223372036854775808", CgroupVersion::kV2},
      {"9223372036854775808", CgroupVersion::kV1},
  };
  for (const Case& c : cases) {
    auto parsed = parse_memory_limit(c.text, c.version);
    ASSERT_FALSE(parsed.has_value()) << "accepted \"" << c.text << "\"";
    EXPECT_EQ(parsed.error(), "\"" + std::string{c.text} + "\" is not a limit " +
                                  std::string{describe(c.version)} + " writes");
  }
}

TEST(HierarchicalLimitTest, TheOneLineIsFoundAmongTheStatistics) {
  auto text = hierarchical_limit_text({"cache 1", "rss 2", "hierarchical_memory_limit 14343688192",
                                       "hierarchical_memsw_limit 9223372036854771712"});
  ASSERT_TRUE(text.has_value()) << text.error();
  EXPECT_EQ(text.value(), "14343688192");

  auto missing = hierarchical_limit_text({"cache 1", "hierarchical_memsw_limit 1"});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error(), "no hierarchical_memory_limit line");
  auto twice =
      hierarchical_limit_text({"hierarchical_memory_limit 1", "hierarchical_memory_limit 2"});
  ASSERT_FALSE(twice.has_value());
  EXPECT_EQ(twice.error(), "hierarchical_memory_limit appears twice");
}

// --- read_memory_budget, on built trees ----------------------------------------------

class BudgetTest : public ::testing::Test {
 protected:
  MachineTree tree{::testing::UnitTest::GetInstance()->current_test_info()->name()};
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  config::MemoryBudget budget() {
    auto result = read_memory_budget(filesystem, tree.root());
    EXPECT_TRUE(result.has_value()) << describe(result.error());
    return result.value_or(config::MemoryBudget{});
  }
  DiscoveryFailure refused() {
    auto result = read_memory_budget(filesystem, tree.root());
    EXPECT_FALSE(result.has_value()) << "read a budget it should have refused";
    return result.has_value() ? DiscoveryFailure{} : result.error();
  }
};

TEST_F(BudgetTest, NoMemoryCgroupLeavesMemAvailableAlone) {
  tree.meminfo(2 * 1024 * 1024);
  tree.write("/proc/self/cgroup", "1:cpu:/\n");
  const config::MemoryBudget read = budget();
  EXPECT_EQ(read.available_bytes, 2 * kGiB);
  EXPECT_FALSE(read.cgroup_limit_bytes.has_value());
  EXPECT_EQ(config::ceiling_of(read), 2 * kGiB);
}

TEST_F(BudgetTest, V1TakesTheKernelsHierarchicalLimitNotTheLeafsOwnFile) {
  // The leaf's own memory.limit_in_bytes is the unlimited sentinel; the
  // parent's limit binds it, and memory.stat says so.
  tree.meminfo(15632336);
  tree.v1("/jobs/a1/shell", "14343688192");
  const config::MemoryBudget read = budget();
  EXPECT_EQ(read.available_bytes, 15632336 * kKiB);
  EXPECT_EQ(read.cgroup_limit_bytes, std::optional<std::uint64_t>{14343688192});
  EXPECT_EQ(config::ceiling_of(read), 14343688192U) << "the lower of the two";

  tree.v1("/jobs/a1/shell", MachineTree::kSentinel);
  EXPECT_FALSE(budget().cgroup_limit_bytes.has_value()) << "unlimited all the way up";
}

TEST_F(BudgetTest, V1InAContainerReadsTheMountItself) {
  tree.meminfo(4 * 1024 * 1024);
  tree.write("/proc/self/cgroup", "4:memory:/docker/ab12\n0::/\n");
  tree.write("/proc/self/mountinfo",
             "1234 1230 0:33 /docker/ab12 /sys/fs/cgroup/memory ro master:15 - cgroup cgroup "
             "rw,memory\n");
  tree.v1_directory("", "1073741824");
  EXPECT_EQ(budget().cgroup_limit_bytes, std::optional<std::uint64_t>{kGiB});
}

TEST_F(BudgetTest, V2TakesTheLowestMemoryMaxFromTheCgroupUpToItsMount) {
  tree.meminfo(8 * 1024 * 1024);
  tree.v2("/a/b/c");
  tree.write("/sys/fs/cgroup/a/b/c/memory.max", "max\n");
  tree.write("/sys/fs/cgroup/a/b/memory.max", "1073741824\n");
  tree.write("/sys/fs/cgroup/a/memory.max", "2147483648\n");
  // No memory.max at the hierarchy's root: the kernel publishes none there.
  EXPECT_EQ(budget().cgroup_limit_bytes, std::optional<std::uint64_t>{kGiB})
      << "the parent's limit binds a leaf that says max";

  tree.write("/sys/fs/cgroup/a/b/c/memory.max", "536870912\n");
  EXPECT_EQ(budget().cgroup_limit_bytes, std::optional<std::uint64_t>{kGiB / 2})
      << "and the leaf's own, when it is the lowest";

  tree.write("/sys/fs/cgroup/a/b/c/memory.max", "max\n");
  tree.write("/sys/fs/cgroup/a/b/memory.max", "max\n");
  tree.write("/sys/fs/cgroup/a/memory.max", "max\n");
  EXPECT_FALSE(budget().cgroup_limit_bytes.has_value());
}

TEST_F(BudgetTest, V2LevelsWithoutMemoryMaxSetNoLimitThere) {
  // A cgroup whose parent does not enable the memory controller for it has no
  // memory.max; nor has the root. Neither is a failure.
  tree.meminfo(8 * 1024 * 1024);
  tree.v2("/a/b");
  tree.write("/sys/fs/cgroup/a/memory.max", "1073741824\n");
  EXPECT_EQ(budget().cgroup_limit_bytes, std::optional<std::uint64_t>{kGiB});
  tree.remove("/sys/fs/cgroup/a/memory.max");
  EXPECT_FALSE(budget().cgroup_limit_bytes.has_value());
}

TEST_F(BudgetTest, MeminfoInEachStateItCanBeIn) {
  tree.v2("/a");
  // Absent: no /proc at all is not "no memory".
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_EQ(failure.subject, tree.at("/proc/meminfo"));

  // Present, with no MemAvailable: absent too, and the words say which kind.
  tree.write("/proc/meminfo", "MemTotal:       16479424 kB\nMemFree:  1 kB\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kAbsent);
  EXPECT_NE(failure.detail.find("3.14"), std::string::npos) << failure.detail;

  tree.write("/proc/meminfo", "MemAvailable:   lots kB\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.subject, tree.at("/proc/meminfo"));

  tree.make_directory_of("/proc/meminfo");
  EXPECT_EQ(refused().availability, Availability::kUnreadable);
}

TEST_F(BudgetTest, ACgroupThatCannotBeLocatedIsAFailureNeverNoLimit) {
  tree.meminfo(1024);
  tree.v2("/a");
  tree.remove("/sys/fs/cgroup/a");
  const DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/a/cgroup.controllers"));
}

TEST_F(BudgetTest, AV2LimitThatCannotBeReadIsAFailureOfItsOwnKind) {
  tree.meminfo(1024);
  tree.v2("/a");
  tree.write("/sys/fs/cgroup/a/memory.max", "lots\n");
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/a/memory.max"));
  EXPECT_EQ(failure.detail, "\"lots\" is not a limit cgroup v2 writes");

  tree.make_directory_of("/sys/fs/cgroup/memory.max");
  tree.write("/sys/fs/cgroup/a/memory.max", "max\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kUnreadable)
      << "an unreadable ancestor is not a level without a limit";
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/memory.max"));
}

TEST_F(BudgetTest, AV1StatThatCannotBeReadIsAFailureOfItsOwnKind) {
  tree.meminfo(1024);
  tree.v1("/a", "1073741824");
  tree.remove("/sys/fs/cgroup/memory/a/memory.stat");
  DiscoveryFailure failure = refused();
  EXPECT_EQ(failure.availability, Availability::kAbsent)
      << "absent, and still a failure: every v1 memory cgroup carries memory.stat";
  EXPECT_EQ(failure.subject, tree.at("/sys/fs/cgroup/memory/a/memory.stat"));

  tree.write("/sys/fs/cgroup/memory/a/memory.stat", "cache 1\nrss 2\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.detail, "no hierarchical_memory_limit line");

  tree.write("/sys/fs/cgroup/memory/a/memory.stat", "hierarchical_memory_limit max\n");
  failure = refused();
  EXPECT_EQ(failure.availability, Availability::kMalformed);
  EXPECT_EQ(failure.detail, "\"max\" is not a limit cgroup v1 writes");

  tree.make_directory_of("/sys/fs/cgroup/memory/a/memory.stat");
  EXPECT_EQ(refused().availability, Availability::kUnreadable);
}

// --- denials, aimed through the fake ------------------------------------------------

TEST(BudgetDeniedTest, EachSourceDeniedIsDeniedAndNamed) {
  using testing::kServedControllers;
  using testing::kServedMembership;
  using testing::kServedMountinfo;
  const std::string meminfo = "MemAvailable:   1048576 kB\n";
  struct Case {
    std::vector<testing::Served> files;
    std::string subject;
  };
  const Case cases[] = {
      {{{"", EACCES}}, "/proc/meminfo"},
      {{{meminfo}, {"", EACCES}}, "/proc/self/cgroup"},
      {{{meminfo}, {kServedMembership}, {kServedMountinfo}, {kServedControllers}, {"", EACCES}},
       "/sys/fs/cgroup/run.scope/memory.max"},
      {{{meminfo},
        {kServedMembership},
        {kServedMountinfo},
        {kServedControllers},
        {"max\n"},
        {"", EPERM}},
       "/sys/fs/cgroup/memory.max"},
      {{{meminfo},
        {"4:memory:/a\n"},
        {"41 37 0:33 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n"},
        {"9223372036854771712\n"},
        {"", EACCES}},
       "/sys/fs/cgroup/memory/a/memory.stat"},
  };
  for (const Case& c : cases) {
    loadforge::testing::FakeSyscalls syscalls;
    serve(syscalls, c.files);
    platform::FileSystem filesystem(syscalls);
    auto result = read_memory_budget(filesystem, "");
    ASSERT_FALSE(result.has_value()) << c.subject;
    EXPECT_EQ(result.error().availability, Availability::kDenied) << c.subject;
    EXPECT_EQ(result.error().subject, c.subject);
    EXPECT_TRUE(syscalls.all_descriptors_closed());
  }
}

// --- the real machine (T8) ----------------------------------------------------------

TEST(BudgetRealTest, ThisMachinesBudgetIsReadAndAgreesWithTheKernelsOtherFigures) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto result = read_memory_budget(filesystem, kMachineRoot);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const config::MemoryBudget budget = result.value();

  // MemAvailable against an independent source: sysconf's physical page
  // count, which the kernel answers from its own total, not from meminfo's
  // text. What can be allocated is more than nothing and no more than there is.
  const auto pages = static_cast<std::uint64_t>(::sysconf(_SC_PHYS_PAGES));
  const auto page_size = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  EXPECT_GT(budget.available_bytes, 0U);
  EXPECT_LE(budget.available_bytes, pages * page_size);

  auto located = locate_memory_cgroup(filesystem, kMachineRoot);
  ASSERT_TRUE(located.has_value()) << describe(located.error());
  if (!located.value().has_value() || located.value()->version != CgroupVersion::kV1) {
    return;  // v2 publishes no second figure to compare; the walk is the reading.
  }
  // v1: the kernel's hierarchical figure can be no higher than any limit set
  // on a level visible from here, each read from its own memory.limit_in_bytes.
  for (const std::string& directory : directories_upward(*located.value())) {
    topology::Source level(filesystem, directory + "/memory.limit_in_bytes");
    auto text = level.text();
    ASSERT_TRUE(text.has_value()) << describe(text.error());
    auto own = parse_memory_limit(text.value(), CgroupVersion::kV1);
    ASSERT_TRUE(own.has_value()) << own.error();
    if (own.value().has_value()) {
      ASSERT_TRUE(budget.cgroup_limit_bytes.has_value()) << directory << " sets a limit";
      EXPECT_LE(*budget.cgroup_limit_bytes, *own.value()) << directory;
    }
  }
}

}  // namespace
}  // namespace loadforge::resources
