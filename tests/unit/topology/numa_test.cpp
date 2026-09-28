// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NUMA layout, and the last part of topology discovery.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Trees are built on disk and driven through RealSyscalls, for the reason given
// at length in cpu_topology_test.cpp: a fake would agree with whatever this
// code believes, which is worthless for testing disagreement (F21).

#include "topology/numa.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

// --- parse_distances ---------------------------------------------------------

TEST(ParseDistancesTest, TheShapesTheKernelWrites) {
  EXPECT_EQ(parse_distances("10").value_or({}), (std::vector<std::uint32_t>{10}));
  EXPECT_EQ(parse_distances("10 20").value_or({}), (std::vector<std::uint32_t>{10, 20}));
  EXPECT_EQ(parse_distances("10 21 21 32").value_or({}),
            (std::vector<std::uint32_t>{10, 21, 21, 32}));
}

TEST(ParseDistancesTest, ShapesTheKernelNeverWritesAreRefused) {
  for (const std::string_view text :
       {"", " ", "10 ", " 10", "10  20", "10,20", "10\t20", "ten", "10 x", "-10", "10 999999"}) {
    EXPECT_FALSE(parse_distances(text).has_value()) << "accepted \"" << text << "\"";
  }
  EXPECT_TRUE(parse_distances("10 65535").has_value()) << "exactly the bound";
  EXPECT_FALSE(parse_distances("10 65536").has_value()) << "one past it";
}

// --- parse_node_mem_total ----------------------------------------------------

std::vector<std::string> meminfo_lines(std::uint32_t id, const std::string& total_kb) {
  const std::string p = "Node " + std::to_string(id) + " ";
  return {p + "MemTotal:        " + total_kb + " kB", p + "MemFree:         3911652 kB",
          p + "MemUsed:         1169664 kB", p + "HugePages_Surp:      0"};
}

TEST(ParseNodeMemTotalTest, ReadsTheValueTheKernelWrites) {
  // The line probed from a real machine, padding and all.
  EXPECT_EQ(parse_node_mem_total(meminfo_lines(0, "5081316"), 0).value_or(0), 5081316ULL * 1024ULL);
}

TEST(ParseNodeMemTotalTest, ZeroIsARealTotalOnACpuOnlyNode) {
  EXPECT_EQ(parse_node_mem_total(meminfo_lines(3, "0"), 3).value_or(1), 0U);
}

TEST(ParseNodeMemTotalTest, AFileDescribingAnotherNodeIsRefused) {
  // The kernel prefixes every line with the node's own id. A meminfo naming a
  // different node is not the file we think it is, and that is worth refusing.
  auto result = parse_node_mem_total(meminfo_lines(1, "5081316"), 0);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(std::string{result.error()}.find("different node"), std::string::npos);
}

TEST(ParseNodeMemTotalTest, EveryOtherMalformationHasItsOwnReason) {
  using Lines = std::vector<std::string>;
  struct Case {
    Lines lines;
    std::string_view reason;
  };
  const Case cases[] = {
      {{"Node 0 MemFree: 1 kB"}, "no MemTotal line"},
      {{"MemTotal:        5081316 kB"}, "does not begin"},
      {{"Node x MemTotal:        5081316 kB"}, "does not name a node"},
      {{"Node 0x MemTotal:        5081316 kB"}, "does not name a node"},
      {{"Node 0xMemTotal:        5081316 kB"}, "does not name a node"},
      {{"Node 0 MemTotal:        abc kB"}, "not a number"},
      {{"Node 0 MemTotal:        5081316 MB"}, "not in kB"},
      {{"Node 0 MemTotal:        5081316"}, "not in kB"},
      {{"Node 0 MemTotal:        5081316 kB extra"}, "not in kB"},
      {{"Node 0 MemTotal:        99999999999999999999 kB"}, "larger than any machine"},
      {{}, "no MemTotal line"},
  };
  for (const Case& c : cases) {
    auto result = parse_node_mem_total(c.lines, 0);
    ASSERT_FALSE(result.has_value()) << "accepted " << (c.lines.empty() ? "<empty>" : c.lines[0]);
    EXPECT_NE(std::string{result.error()}.find(c.reason), std::string::npos)
        << "for " << (c.lines.empty() ? "<empty>" : c.lines[0]) << " got: " << result.error();
  }
}

// --- discovery ---------------------------------------------------------------

class NumaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("loadforge-numa-" + std::to_string(::getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(root / "devices" / "system" / "cpu");
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents << "\n";
  }

  std::filesystem::path node_dir(std::uint32_t id) {
    return root / "devices" / "system" / "node" / ("node" + std::to_string(id));
  }

  void set_cpus_online(const std::string& list) {
    write(root / "devices" / "system" / "cpu" / "online", list);
    // Every CPU on its own core in package 0, so the CPU half is always sound.
    //
    // The list is materialised before the loop, and GCC insisted on it: in a
    // range-for, `parse(list).value().ids()` binds the range to a vector living
    // inside a temporary Result, and only the FINAL temporary of a range
    // expression has its lifetime extended in C++20. The Result dies before the
    // first iteration and the loop walks freed memory. -Wdangling-reference was
    // right, and a test that scribbles its fixture from a dangling vector would
    // have been a fine way to spend an afternoon.
    const CpuList cpus = CpuList::parse(list).value();
    for (const std::uint32_t id : cpus.ids()) {
      const auto d =
          root / "devices" / "system" / "cpu" / ("cpu" + std::to_string(id)) / "topology";
      write(d / "physical_package_id", "0");
      write(d / "core_id", std::to_string(id));
      write(d / "thread_siblings_list", std::to_string(id));
    }
  }

  void set_nodes_online(const std::string& list) {
    write(root / "devices" / "system" / "node" / "online", list);
  }

  /// One node directory, exactly as the kernel lays it out: a meminfo of
  /// several prefixed lines, with MemTotal not first.
  void add_node(std::uint32_t id, const std::string& cpulist, const std::string& total_kb,
                const std::string& distance) {
    write(node_dir(id) / "cpulist", cpulist);
    write(node_dir(id) / "distance", distance);
    const std::string p = "Node " + std::to_string(id) + " ";
    write(node_dir(id) / "meminfo", p + "MemFree:         3911652 kB\n" + p + "MemTotal:        " +
                                        total_kb + " kB\n" + p + "MemUsed:         1169664 kB");
  }

  core::Result<NumaTopology, DiscoveryFailure> discover() {
    auto cpus = CpuTopology::discover(filesystem, root.string());
    EXPECT_TRUE(cpus.has_value()) << "the CPU half must be sound before nodes mean anything";
    return NumaTopology::discover(filesystem, cpus.value(), root.string());
  }

  std::filesystem::path root;
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

TEST_F(NumaTest, OneNodeHoldingEveryCpu) {
  // The machine this was written against, values and all.
  set_cpus_online("0-3");
  set_nodes_online("0");
  add_node(0, "0-3", "5081316", "10");

  auto numa = discover();
  ASSERT_TRUE(numa.has_value()) << describe(numa.error());
  EXPECT_TRUE(numa.value().exposed());
  ASSERT_EQ(numa.value().count(), 1U);
  const NumaNode& node = numa.value().nodes().front();
  EXPECT_EQ(node.id, 0U);
  EXPECT_EQ(node.cpus.size(), 4U);
  EXPECT_EQ(node.memory_bytes, 5081316ULL * 1024ULL);
  EXPECT_EQ(node.distances, (std::vector<std::uint32_t>{10}));
  ASSERT_NE(numa.value().node_of(2), nullptr);
  EXPECT_EQ(numa.value().node_of(2)->id, 0U);
  EXPECT_EQ(numa.value().node_of(9), nullptr) << "a CPU discovery never saw has no node";
}

TEST_F(NumaTest, TwoNodesPartitioningTheCpus) {
  set_cpus_online("0-3");
  set_nodes_online("0-1");
  add_node(0, "0-1", "8000000", "10 20");
  add_node(1, "2-3", "8000000", "20 10");

  auto numa = discover();
  ASSERT_TRUE(numa.has_value()) << describe(numa.error());
  ASSERT_EQ(numa.value().count(), 2U);
  EXPECT_EQ(numa.value().node_of(1)->id, 0U);
  EXPECT_EQ(numa.value().node_of(2)->id, 1U);
  EXPECT_EQ(numa.value().nodes()[1].distances, (std::vector<std::uint32_t>{20, 10}));
}

TEST_F(NumaTest, AMemoryOnlyNodeHasNoCpusAndThatIsNotAnError) {
  set_cpus_online("0-1");
  set_nodes_online("0-1");
  add_node(0, "0-1", "8000000", "10 20");
  add_node(1, "", "8000000", "20 10");  // Every CPU is on node 0; node 1 is memory.

  auto numa = discover();
  ASSERT_TRUE(numa.has_value()) << describe(numa.error());
  ASSERT_EQ(numa.value().count(), 2U);
  EXPECT_TRUE(numa.value().nodes()[1].cpus.empty());
  EXPECT_EQ(numa.value().nodes()[1].memory_bytes, 8000000ULL * 1024ULL);
}

TEST_F(NumaTest, AKernelPublishingNoNodesIsNotExposedRatherThanFailedOrInvented) {
  // No `node/` directory at all. Neither a refusal nor a fabricated single
  // node: a layout with nothing in it, and node_of answers null, because
  // there is no node to default to.
  set_cpus_online("0-3");

  auto numa = discover();
  ASSERT_TRUE(numa.has_value()) << describe(numa.error());
  EXPECT_FALSE(numa.value().exposed());
  EXPECT_EQ(numa.value().count(), 0U);
  EXPECT_EQ(numa.value().node_of(0), nullptr);
}

TEST_F(NumaTest, AnUnreadableOnlineFileIsStillAFailure) {
  // Absent is the one failure that is an answer. A directory in its place is
  // a real EISDIR and must not be mistaken for "not exposed".
  set_cpus_online("0");
  std::filesystem::create_directories(root / "devices" / "system" / "node" / "online");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(numa.error().availability, Availability::kUnreadable);
}

TEST_F(NumaTest, NoNodeOnlineIsImpossibleRatherThanEmpty) {
  set_cpus_online("0");
  set_nodes_online("");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("running on one"), std::string::npos);
}

TEST_F(NumaTest, EachRequiredAttributeMissingIsASourceFailure) {
  for (const char* missing : {"cpulist", "meminfo", "distance"}) {
    TearDown();
    SetUp();
    set_cpus_online("0");
    set_nodes_online("0");
    add_node(0, "0", "1000", "10");
    ASSERT_TRUE(std::filesystem::remove(node_dir(0) / missing));

    auto numa = discover();
    ASSERT_FALSE(numa.has_value()) << "succeeded without " << missing;
    EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kSource) << missing;
    EXPECT_EQ(numa.error().availability, Availability::kAbsent) << missing;
  }
}

TEST_F(NumaTest, ANodeClaimingAnOfflineCpuIsRefused) {
  set_cpus_online("0");
  set_nodes_online("0");
  add_node(0, "0-1", "1000", "10");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("not online"), std::string::npos);
}

TEST_F(NumaTest, AnOnlineCpuBelongingToNoNodeIsRefused) {
  set_cpus_online("0-1");
  set_nodes_online("0");
  add_node(0, "0", "1000", "10");  // cpu1 is online and nobody's.

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("belongs to no node"), std::string::npos);
}

TEST_F(NumaTest, ACpuClaimedByTwoNodesIsRefused) {
  set_cpus_online("0-1");
  set_nodes_online("0-1");
  add_node(0, "0-1", "1000", "10 20");
  add_node(1, "1", "1000", "20 10");  // cpu1 is on both.

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("claimed by 2 nodes"), std::string::npos);
}

TEST_F(NumaTest, ADistanceRowOfTheWrongLengthIsRefused) {
  set_cpus_online("0-1");
  set_nodes_online("0-1");
  add_node(0, "0", "1000", "10 20 30");  // Three entries, two nodes online.
  add_node(1, "1", "1000", "20 10");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("3 distance(s) but 2 node(s)"), std::string::npos);
}

TEST_F(NumaTest, AMalformedDistanceRowIsRefusedWithTheParsersReason) {
  set_cpus_online("0");
  set_nodes_online("0");
  add_node(0, "0", "1000", "10,");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("single spaces"), std::string::npos);
}

TEST_F(NumaTest, AMeminfoDescribingAnotherNodeIsRefused) {
  set_cpus_online("0");
  set_nodes_online("0");
  add_node(0, "0", "1000", "10");
  write(node_dir(0) / "meminfo", "Node 7 MemTotal:        1000 kB");

  auto numa = discover();
  ASSERT_FALSE(numa.has_value());
  EXPECT_EQ(numa.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(numa.error()).find("different node"), std::string::npos);
}

// --- the real machine (T8) ---------------------------------------------------

TEST(RealNumaTest, TheRunningMachinesNodesPartitionItsCpus) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  auto cpus = CpuTopology::discover(filesystem);
  ASSERT_TRUE(cpus.has_value()) << describe(cpus.error());
  auto numa = NumaTopology::discover(filesystem, cpus.value());
  ASSERT_TRUE(numa.has_value()) << describe(numa.error());

  if (!numa.value().exposed()) {
    return;  // A kernel without NUMA is a legitimate machine; nothing to corroborate.
  }

  // Corroboration from an independent source (F21): the kernel also links
  // each CPU to its node from the CPU's side, as cpu/cpuN/nodeM. That link is
  // not what discovery read, so agreement is evidence -- and this is a test
  // that can fail, not a rule that can refuse a user's machine.
  for (const NumaNode& node : numa.value().nodes()) {
    for (const std::uint32_t cpu : node.cpus.ids()) {
      Source link(filesystem, "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/node" +
                                  std::to_string(node.id) + "/cpulist");
      auto through_cpu = link.cpu_list();
      ASSERT_TRUE(through_cpu.has_value()) << describe(through_cpu.error());
      EXPECT_EQ(through_cpu.value(), node.cpus)
          << "cpu" << cpu << " links to node" << node.id << " but sees a different cpulist";
    }
    EXPECT_EQ(node.distances.size(), numa.value().count());
  }

  // Deliberately NOT asserted: that node memory sums to /proc/meminfo's
  // MemTotal. On the VM this was written against node0 reported 4962 MiB
  // against a system total of 16095 MiB, with every online memory block under
  // node0. That is what the kernel says, and a test that called it wrong would
  // be the one that is wrong.
}

}  // namespace
}  // namespace loadforge::topology
