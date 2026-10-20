// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snapshot: the three halves of discovery taken together, once, and
// described in one line. What each half reads and refuses is its own tests'
// subject; these check that the snapshot is whole or not taken, that the
// first half to refuse is the answer, and what the line says.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Trees are built on disk and read through RealSyscalls, as for every other
// part of discovery: one refusal per half, each made by the tree itself.

#include "topology/snapshot.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

class SnapshotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("loadforge-snapshot-" + std::to_string(::getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(cpu_root());
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  [[nodiscard]] std::filesystem::path cpu_root() const {
    return root / "devices" / "system" / "cpu";
  }
  [[nodiscard]] std::filesystem::path node_root() const {
    return root / "devices" / "system" / "node";
  }

  static void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents << "\n";
  }

  /// CPUs 0..count-1 online, each its own core in package 0.
  void add_cpus(std::uint32_t count) {
    write(cpu_root() / "online", count == 1 ? "0" : "0-" + std::to_string(count - 1));
    for (std::uint32_t id = 0; id < count; ++id) {
      const auto topology = cpu_root() / ("cpu" + std::to_string(id)) / "topology";
      write(topology / "physical_package_id", "0");
      write(topology / "core_id", std::to_string(id));
      write(topology / "thread_siblings_list", std::to_string(id));
    }
  }

  void add_cache(std::uint32_t cpu, std::uint32_t index, const std::string& level,
                 const std::string& type, const std::string& shared) {
    const auto d =
        cpu_root() / ("cpu" + std::to_string(cpu)) / "cache" / ("index" + std::to_string(index));
    write(d / "level", level);
    write(d / "type", type);
    write(d / "shared_cpu_list", shared);
    write(d / "size", "32K");
    write(d / "coherency_line_size", "64");
  }

  /// Two CPUs with private L1d, L1i and L2 and one shared L3, on one node.
  void build_whole_machine() {
    add_cpus(2);
    for (std::uint32_t id = 0; id < 2; ++id) {
      const std::string self = std::to_string(id);
      add_cache(id, 0, "1", "Data", self);
      add_cache(id, 1, "1", "Instruction", self);
      add_cache(id, 2, "2", "Unified", self);
      add_cache(id, 3, "3", "Unified", "0-1");
    }
    write(node_root() / "online", "0");
    write(node_root() / "node0" / "cpulist", "0-1");
    write(node_root() / "node0" / "distance", "10");
    write(node_root() / "node0" / "meminfo", "Node 0 MemTotal:        8000000 kB");
  }

  core::Result<Snapshot, DiscoveryFailure> take() {
    return take_snapshot(filesystem, root.string());
  }

  std::filesystem::path root;
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

TEST_F(SnapshotTest, AWholeMachineIsDescribedInOneLine) {
  build_whole_machine();
  auto snapshot = take();
  ASSERT_TRUE(snapshot.has_value()) << describe(snapshot.error());
  EXPECT_EQ(snapshot.value().cpus.logical_cpu_count(), 2U);
  EXPECT_EQ(snapshot.value().caches.count(), 7U) << "two of each private cache, one shared L3";
  EXPECT_EQ(snapshot.value().numa.count(), 1U);
  EXPECT_EQ(describe(snapshot.value()),
            "2 logical CPU(s) on 2 core(s) in 1 package(s); 7 cache(s), deepest L3; "
            "1 NUMA node(s)");
}

TEST_F(SnapshotTest, WhatTheKernelDoesNotPublishIsSaidToBeNotExposedNeverZero) {
  add_cpus(1);
  auto snapshot = take();
  ASSERT_TRUE(snapshot.has_value()) << describe(snapshot.error());
  EXPECT_EQ(describe(snapshot.value()),
            "1 logical CPU(s) on 1 core(s) in 1 package(s); caches not exposed; NUMA not exposed");
}

TEST_F(SnapshotTest, CpusThatCannotBeDiscoveredRefuseTheSnapshotInTheirOwnWords) {
  // No cpu/online: nothing else can be read against CPUs that are not known.
  build_whole_machine();
  std::filesystem::remove(cpu_root() / "online");
  auto snapshot = take();
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().availability, Availability::kAbsent);
  EXPECT_EQ(snapshot.error().subject, (cpu_root() / "online").string());
}

TEST_F(SnapshotTest, CachesThatCannotBeDiscoveredRefuseTheSnapshotThoughCpusWere) {
  build_whole_machine();
  write(cpu_root() / "cpu1" / "cache" / "index2" / "level", "two");
  auto snapshot = take();
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().availability, Availability::kMalformed) << describe(snapshot.error());
  EXPECT_EQ(snapshot.error().subject,
            (cpu_root() / "cpu1" / "cache" / "index2" / "level").string());
}

TEST_F(SnapshotTest, ANumaLayoutThatContradictsItselfRefusesTheSnapshotLast) {
  // Every CPU and cache is sound; the published node list is empty, which the
  // running code itself disproves.
  build_whole_machine();
  write(node_root() / "online", "");
  auto snapshot = take();
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().kind, DiscoveryFailure::Kind::kContradiction)
      << describe(snapshot.error());
  EXPECT_EQ(snapshot.error().subject, (node_root() / "online").string());
}

}  // namespace
}  // namespace loadforge::topology
