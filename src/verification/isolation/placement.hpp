// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ISOLATION_PLACEMENT_HPP
#define LOADFORGE_VERIFICATION_ISOLATION_PLACEMENT_HPP

#include <cstdint>
#include <vector>

#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"

namespace loadforge::verification::isolation {

/// The logical CPUs on physical cores other than `cpu`'s, in the order the
/// ladder's second rung should try them: nearest first.
///
/// A pure function of the snapshot -- the topology taken once, before the
/// fork (PLAN §4.1) -- so the choice can be stated and tested without a
/// machine, and two isolations of one run cannot disagree about it.
///
/// WHY THE NEAREST OTHER CORE, AND NOT THE FARTHEST
/// ------------------------------------------------
/// The rung asks one question: is the fault in this core? Its two readings
/// (verification.md §7) are "clean elsewhere: consistent with a core-local
/// fault" and "reproduced elsewhere: consistent with memory, uncore or power
/// delivery". Both hold only if the other core shares everything but the
/// core: the same NUMA node, so the same memory, and as much of the cache
/// hierarchy as any other core shares. §7 lists a different NUMA placement and
/// a different shared-cache or cluster topology among the things that make
/// "fails on 5, passes on 11" mean something other than a core-local fault.
/// A core across a cache domain or a node would add exactly those confounds:
/// a fault in the original's L3 slice or its node's memory would come out
/// clean there and be read as core-local. So the order holds everything
/// constant that the topology lets it hold constant, and changes only the
/// core (docs/journal/10-isolation.md, decision 2).
///
/// THE ORDER
/// ---------
/// Every online CPU whose (package, core) differs from `cpu`'s -- never an SMT
/// sibling, which shares the core under test -- ranked by, in turn:
///
///   1. the same NUMA node as `cpu` before another node. A snapshot without
///      NUMA has no node to differ in, and ranks every CPU alike here;
///   2. the lowest cache level shared with `cpu`, lowest first: a core that
///      shares an L2 cluster before one that shares only the L3, before one
///      that shares no cache at all. Caches not exposed share nothing;
///   3. a core of the same shape before one of another: the same number of
///      logical CPUs, and the same private caches by level, type and size. The
///      snapshot does not name core kinds; on a heterogeneous package (P and E
///      cores, big and LITTLE) the shape is what tells them apart, and a core of
///      the original's kind is the one that changes nothing but the core;
///   4. the CPU id, ascending, so the order is total and reproducible.
///
/// Empty when `cpu` is not a CPU of the snapshot, since nothing can be said to
/// be on another core than an unknown one, and when the machine has one
/// physical core. An empty list is the placement offering no other core, and
/// the ladder skips that rung.
[[nodiscard]] std::vector<std::uint32_t> other_cores(const topology::Snapshot& machine,
                                                     std::uint32_t cpu);

/// The placement of a unit that ran on `cpu` with `threads` threads, with
/// other_cores(machine, cpu) as the cores the ladder may move it to.
[[nodiscard]] Placement placement_for(const topology::Snapshot& machine, std::uint32_t cpu,
                                      std::uint32_t threads);

}  // namespace loadforge::verification::isolation

#endif
