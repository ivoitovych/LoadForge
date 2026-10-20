// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/isolation/placement.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

#include "topology/cache.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/numa.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"

namespace loadforge::verification::isolation {
namespace {

using topology::Cache;
using topology::LogicalCpu;

/// The level two CPUs share when they share no cache: above every real one,
/// so it ranks last.
constexpr std::uint32_t kNoSharedCache = std::numeric_limits<std::uint32_t>::max();

/// A cache private to one core, as far as its shape goes: what it is and how
/// large. The size is optional, as the snapshot holds it; two caches of
/// unknown size are alike in what is known of them.
using PrivateCache = std::tuple<std::uint32_t, topology::CacheType, std::optional<std::uint64_t>>;

/// What a core looks like from the snapshot: its thread count and its private
/// caches, in the snapshot's order (by level, then type).
struct Shape {
  std::size_t threads = 0;
  std::vector<PrivateCache> caches;
};

bool same(const Shape& lhs, const Shape& rhs) {
  return lhs.threads == rhs.threads && lhs.caches == rhs.caches;
}

/// One candidate's place in the order: each criterion such that less is
/// nearer, and the id last so that the order is total.
using Rank = std::tuple<bool, std::uint32_t, bool, std::uint32_t>;

const LogicalCpu* find_cpu(const topology::CpuTopology& cpus, std::uint32_t id) noexcept {
  const auto& all = cpus.cpus();
  const auto found =
      std::find_if(all.begin(), all.end(), [id](const LogicalCpu& each) { return each.id == id; });
  return found == all.end() ? nullptr : &*found;
}

/// The lowest level of any cache serving both CPUs. The hierarchy lists its
/// caches by level, so the first one found is the lowest.
std::uint32_t lowest_shared_level(const topology::CacheHierarchy& caches, std::uint32_t one,
                                  std::uint32_t other) noexcept {
  for (const Cache& cache : caches.caches()) {
    if (cache.shared_with.contains(one) && cache.shared_with.contains(other)) {
      return cache.kind.level;
    }
  }
  return kNoSharedCache;
}

/// Whether a cache serving `cpu` serves no CPU outside its core: part of the
/// core rather than of a cluster, a package or the machine.
bool private_to_core(const Cache& cache, const LogicalCpu& cpu) {
  const auto& sharers = cache.shared_with.ids();
  return std::all_of(sharers.begin(), sharers.end(),
                     [&cpu](std::uint32_t sharer) { return cpu.thread_siblings.contains(sharer); });
}

Shape shape_of(const topology::CacheHierarchy& caches, const LogicalCpu& cpu) {
  Shape shape;
  shape.threads = cpu.thread_siblings.size();
  for (const Cache& cache : caches.caches()) {
    if (cache.shared_with.contains(cpu.id) && private_to_core(cache, cpu)) {
      shape.caches.emplace_back(cache.kind.level, cache.kind.type, cache.size_bytes);
    }
  }
  return shape;
}

}  // namespace

std::vector<std::uint32_t> other_cores(const topology::Snapshot& machine, std::uint32_t cpu) {
  const LogicalCpu* origin = find_cpu(machine.cpus, cpu);
  if (origin == nullptr) {
    return {};
  }
  const topology::NumaNode* node = machine.numa.node_of(cpu);
  const Shape shape = shape_of(machine.caches, *origin);

  std::vector<Rank> ranked;
  for (const LogicalCpu& candidate : machine.cpus.cpus()) {
    if (topology::key(candidate.core) == topology::key(origin->core)) {
      continue;
    }
    ranked.emplace_back(machine.numa.node_of(candidate.id) != node,
                        lowest_shared_level(machine.caches, cpu, candidate.id),
                        !same(shape_of(machine.caches, candidate), shape), candidate.id);
  }
  std::sort(ranked.begin(), ranked.end());

  std::vector<std::uint32_t> order;
  order.reserve(ranked.size());
  for (const Rank& rank : ranked) {
    order.push_back(std::get<3>(rank));
  }
  return order;
}

Placement placement_for(const topology::Snapshot& machine, std::uint32_t cpu,
                        std::uint32_t threads) {
  return Placement{cpu, threads, other_cores(machine, cpu)};
}

}  // namespace loadforge::verification::isolation
