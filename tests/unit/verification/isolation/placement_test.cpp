// SPDX-License-Identifier: GPL-3.0-or-later
//
// Choosing the other core: a pure function of the topology snapshot, tested
// on the symmetric, SMT, heterogeneous and single-core layouts of layouts.hpp,
// on layouts that separate each rule of the order from the next, and on this
// machine's own /sys.

#include "verification/isolation/placement.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "unit/verification/isolation/layouts.hpp"

namespace loadforge::verification::isolation {
namespace {

using Cpus = std::vector<std::uint32_t>;

/// The snapshot of a layout, or nothing -- with the refusal reported -- when
/// discovery would not accept the tree.
std::optional<topology::Snapshot> take(void (*build)(layouts::Tree&), const char* name) {
  layouts::Tree tree(name);
  build(tree);
  auto snapshot = tree.take();
  if (!snapshot.has_value()) {
    ADD_FAILURE() << "discovery refused the " << name
                  << " layout: " << topology::describe(snapshot.error());
    return std::nullopt;
  }
  return std::move(snapshot).value();
}

TEST(OtherCores, SymmetricEveryOtherCoreInIdOrder) {
  const auto machine_snapshot = take(layouts::symmetric, "symmetric");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{1, 2, 3}));
  EXPECT_EQ(other_cores(machine, 1), (Cpus{0, 2, 3}));
  EXPECT_EQ(other_cores(machine, 3), (Cpus{0, 1, 2}));
}

TEST(OtherCores, SmtNeverOffersTheSiblingWhichSharesTheCoreUnderTest) {
  const auto machine_snapshot = take(layouts::smt, "smt");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{1, 2, 3, 5, 6, 7}));
  EXPECT_EQ(other_cores(machine, 4), (Cpus{1, 2, 3, 5, 6, 7}))
      << "the second thread of core 0 is on core 0 too";
  EXPECT_EQ(other_cores(machine, 6), (Cpus{0, 1, 3, 4, 5, 7}));
}

TEST(OtherCores, BigLittleAMoveStaysOnACoreOfTheSameKind) {
  const auto machine_snapshot = take(layouts::big_little, "big-little");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 4), (Cpus{5, 6, 7, 0, 1, 2, 3}))
      << "a big core's nearest are the other big cores, though LITTLE ids come first";
  EXPECT_EQ(other_cores(machine, 1), (Cpus{0, 2, 3, 4, 5, 6, 7}));
}

TEST(OtherCores, HybridAnEfficiencyCoreMovesInsideItsClusterThenToItsKind) {
  const auto machine_snapshot = take(layouts::hybrid, "hybrid");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 4), (Cpus{5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3}))
      << "cluster mates share the L2; the other cluster shares the L3 and the kind";
  EXPECT_EQ(other_cores(machine, 9), (Cpus{8, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3}));
  EXPECT_EQ(other_cores(machine, 0), (Cpus{2, 3, 4, 5, 6, 7, 8, 9, 10, 11}))
      << "a performance core's sibling is never offered; the other performance core first";
  EXPECT_EQ(other_cores(machine, 3), (Cpus{0, 1, 4, 5, 6, 7, 8, 9, 10, 11}));
}

TEST(OtherCores, ASingleCoreOffersNothing) {
  const auto machine_snapshot = take(layouts::single_core, "single-core");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_TRUE(other_cores(machine, 0).empty());
}

TEST(OtherCores, ASingleCoreWithTwoThreadsStillOffersNothing) {
  const auto machine_snapshot = take(layouts::single_core_smt, "single-core-smt");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_TRUE(other_cores(machine, 0).empty());
  EXPECT_TRUE(other_cores(machine, 1).empty());
}

TEST(OtherCores, TwoSocketsTheSameNodeFirstAndCoreIdsCountPerPackage) {
  const auto machine_snapshot = take(layouts::two_sockets, "two-sockets");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{1, 2, 3}))
      << "cpu 2 is core 0 too, of package 1: another core";
  EXPECT_EQ(other_cores(machine, 2), (Cpus{3, 0, 1}));
}

TEST(OtherCores, TheNodeRanksAboveTheCache) {
  const auto machine_snapshot = take(layouts::node_before_cache, "node-before-cache");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{2, 1, 3}));
  EXPECT_EQ(other_cores(machine, 3), (Cpus{1, 2, 0}));
}

TEST(OtherCores, TheCacheRanksAboveTheShapeAndTheShapeAboveTheId) {
  const auto machine_snapshot = take(layouts::cache_before_shape, "cache-before-shape");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{1, 3, 2}));
}

TEST(OtherCores, AShapeCountsTheCoresThreads) {
  const auto machine_snapshot = take(layouts::uneven_smt, "uneven-smt");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{2, 4, 1}));
  EXPECT_EQ(other_cores(machine, 1), (Cpus{0, 2, 3, 4}));
}

TEST(OtherCores, AShapeIsTheCoresOwnCachesNotItsClusters) {
  const auto machine_snapshot = take(layouts::clusters_of_two_sizes, "two-sizes");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_EQ(other_cores(machine, 0), (Cpus{1, 2, 3, 4, 5}))
      << "cpu 2 and 3 are cpu 0's kind, whatever their cluster's L2 holds";
}

TEST(OtherCores, WithoutCachesOrNodesEveryOtherCoreRanksAlike) {
  const auto machine_snapshot = take(layouts::cpus_only, "cpus-only");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  ASSERT_EQ(machine.caches.count(), 0U);
  ASSERT_FALSE(machine.numa.exposed());
  EXPECT_EQ(other_cores(machine, 1), (Cpus{0, 2}));
}

TEST(OtherCores, ACpuTheSnapshotDoesNotHoldHasNoOtherCore) {
  const auto machine_snapshot = take(layouts::symmetric, "unknown-cpu");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  EXPECT_TRUE(other_cores(machine, 4).empty());
  EXPECT_TRUE(other_cores(machine, 0xFFFFFFFFU).empty());
}

TEST(OtherCores, TheOrderIsAFunctionOfTheSnapshotAlone) {
  const auto first_snapshot = take(layouts::hybrid, "hybrid-first");
  ASSERT_TRUE(first_snapshot.has_value());
  const topology::Snapshot& first = *first_snapshot;
  const auto second_snapshot = take(layouts::hybrid, "hybrid-second");
  ASSERT_TRUE(second_snapshot.has_value());
  const topology::Snapshot& second = *second_snapshot;
  for (std::uint32_t cpu = 0; cpu < 12; ++cpu) {
    EXPECT_EQ(other_cores(first, cpu), other_cores(second, cpu)) << "cpu " << cpu;
    EXPECT_EQ(other_cores(first, cpu), other_cores(first, cpu)) << "cpu " << cpu;
  }
}

TEST(PlacementFor, CarriesTheOriginalAndTheOrder) {
  const auto machine_snapshot = take(layouts::smt, "placement");
  ASSERT_TRUE(machine_snapshot.has_value());
  const topology::Snapshot& machine = *machine_snapshot;
  const Placement placement = placement_for(machine, 5, 4);
  EXPECT_EQ(placement.cpu, 5U);
  EXPECT_EQ(placement.threads, 4U);
  EXPECT_EQ(placement.other_cores, other_cores(machine, 5));
  EXPECT_EQ(placement.other_cores.front(), 0U);
  EXPECT_TRUE(placement_for(machine, 99, 1).other_cores.empty());
}

TEST(Layouts, ARefusedTreeIsReportedNotUsed) {
  // The builder's own failure arm: a tree with no CPUs is refused by
  // discovery, which the take() above turns into a test failure.
  layouts::Tree tree("refused");
  const auto snapshot = tree.take();
  ASSERT_FALSE(snapshot.has_value());
  EXPECT_EQ(snapshot.error().availability, topology::Availability::kAbsent);
}

// The real machine. The trees above are this module's model of what the
// kernel writes; here the kernel writes it. Every CPU's list must be exactly
// the CPUs on other physical cores, each once, and the first must be on the
// same node whenever the node holds another core (F21).
TEST(OtherCoresOnThisMachine, EveryListIsTheOtherCoresAndTheFirstIsNearest) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  const auto snapshot = topology::take_snapshot(filesystem);
  ASSERT_TRUE(snapshot.has_value()) << topology::describe(snapshot.error());
  const topology::Snapshot& machine = snapshot.value();
  for (const topology::LogicalCpu& cpu : machine.cpus.cpus()) {
    std::set<std::uint32_t> expected;
    bool node_has_another_core = false;
    for (const topology::LogicalCpu& other : machine.cpus.cpus()) {
      if (topology::key(other.core) != topology::key(cpu.core)) {
        expected.insert(other.id);
        node_has_another_core =
            node_has_another_core || machine.numa.node_of(other.id) == machine.numa.node_of(cpu.id);
      }
    }
    const auto order = other_cores(machine, cpu.id);
    EXPECT_EQ(std::set<std::uint32_t>(order.begin(), order.end()), expected) << "cpu " << cpu.id;
    EXPECT_EQ(order.size(), expected.size()) << "each CPU once";
    if (node_has_another_core) {
      EXPECT_EQ(machine.numa.node_of(order.front()), machine.numa.node_of(cpu.id))
          << "cpu " << cpu.id;
    }
  }
}

}  // namespace
}  // namespace loadforge::verification::isolation
