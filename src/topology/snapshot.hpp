// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_SNAPSHOT_HPP
#define LOADFORGE_TOPOLOGY_SNAPSHOT_HPP

#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cache.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/numa.hpp"

namespace loadforge::topology {

/// The machine, described once: its CPUs, its caches and its NUMA layout.
///
/// docs/PLAN.md §4.1: topology is discovered ONCE, by the controller, BEFORE
/// it forks, and is immutable and shared from then on. Once, so a run cannot
/// disagree with itself about what the machine is, and a worker re-spawned
/// later sees the same description as the first. Before the fork, because it
/// is the frame every later report is attributed against -- a death on CPU 11,
/// node 1 -- and a frame discovered afterwards could describe a machine the
/// workers never ran on. The state machine of IMPLEMENTATION §4 M1 draws it as
/// the transition out of START.
///
/// Whole or not at all: each half is discovered against the CPUs, and a
/// snapshot missing a half would be a description nobody could tell was
/// partial.
struct Snapshot {
  CpuTopology cpus;
  CacheHierarchy caches;
  NumaTopology numa;
};

/// Takes the snapshot: the CPUs first, then the caches and the NUMA layout
/// against them. The first half that refuses is the answer, in its own words.
[[nodiscard]] core::Result<Snapshot, DiscoveryFailure> take_snapshot(
    platform::FileSystem& filesystem, std::string_view sysfs_root = kDefaultSysfsRoot);

/// One line for a report: "4 logical CPU(s) on 2 core(s) in 1 package(s);
/// 6 cache(s), deepest L3; 1 NUMA node(s)". A layout the kernel does not
/// publish is said to be not exposed, never counted as zero.
[[nodiscard]] std::string describe(const Snapshot& snapshot);

}  // namespace loadforge::topology

#endif
