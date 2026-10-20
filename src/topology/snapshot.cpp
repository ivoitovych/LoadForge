// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/snapshot.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cache.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/numa.hpp"

namespace loadforge::topology {

core::Result<Snapshot, DiscoveryFailure> take_snapshot(platform::FileSystem& filesystem,
                                                       std::string_view sysfs_root) {
  auto cpus = CpuTopology::discover(filesystem, sysfs_root);
  if (!cpus.has_value()) {
    return cpus.error();
  }
  auto caches = CacheHierarchy::discover(filesystem, cpus.value(), sysfs_root);
  if (!caches.has_value()) {
    return caches.error();
  }
  auto numa = NumaTopology::discover(filesystem, cpus.value(), sysfs_root);
  if (!numa.has_value()) {
    return numa.error();
  }
  // Each value taken on its own line, so that the aggregate is built from
  // moves that cannot throw: built from value() calls, it would carry the
  // cleanup of a half-built snapshot, edges no test can take (journal §1.13).
  CpuTopology&& found_cpus = std::move(cpus).value();
  CacheHierarchy&& found_caches = std::move(caches).value();
  NumaTopology&& found_numa = std::move(numa).value();
  return Snapshot{std::move(found_cpus), std::move(found_caches), std::move(found_numa)};
}

std::string describe(const Snapshot& snapshot) {
  // One allocating step per statement (journal §1.13).
  std::string text = std::to_string(snapshot.cpus.logical_cpu_count());
  text += " logical CPU(s) on ";
  text += std::to_string(snapshot.cpus.physical_core_count());
  text += " core(s) in ";
  text += std::to_string(snapshot.cpus.package_count());
  text += " package(s); ";
  if (snapshot.caches.count() == 0) {
    text += "caches not exposed";
  } else {
    text += std::to_string(snapshot.caches.count());
    text += " cache(s), deepest L";
    text += std::to_string(snapshot.caches.deepest_level());
  }
  text += "; ";
  if (snapshot.numa.exposed()) {
    text += std::to_string(snapshot.numa.count());
    text += " NUMA node(s)";
  } else {
    text += "NUMA not exposed";
  }
  return text;
}

}  // namespace loadforge::topology
