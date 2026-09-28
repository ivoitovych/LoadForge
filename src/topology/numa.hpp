// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_NUMA_HPP
#define LOADFORGE_TOPOLOGY_NUMA_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::topology {

/// One NUMA node, as the kernel describes it.
struct NumaNode {
  std::uint32_t id = 0;
  /// The CPUs local to this node. Empty on a memory-only node, which is a real
  /// arrangement (a CXL expander, an offlined socket's memory) and not an error.
  CpuList cpus;
  /// The node's own `MemTotal`, in bytes. Zero on a CPU-only node.
  ///
  /// NOT a share of the machine's memory. On the VM this was written against,
  /// node0 -- the only node -- reported 4962 MiB while /proc/meminfo reported
  /// 16095 MiB, with all 128 online memory blocks sitting under node0. The
  /// node's own accounting undercounts its own blocks, and a check that the
  /// nodes sum to the machine would have refused a perfectly ordinary VM. So
  /// there is no total here, and nothing compares this to anything.
  std::uint64_t memory_bytes = 0;
  /// Distance to every online node, in `node/online` order -- so the entry at
  /// this node's own position in that order is its distance to itself. The
  /// kernel writes 10 for local on every machine seen so far; that is not
  /// checked, because "every machine seen so far" is not "every machine".
  std::vector<std::uint32_t> distances;

  [[nodiscard]] friend bool operator==(const NumaNode&, const NumaNode&) = default;
};

/// The machine's NUMA layout: which CPUs are near which memory.
///
/// WHAT "NOT EXPOSED" MEANS, AND WHY IT IS NOT A FAILURE
/// ----------------------------------------------------
/// A kernel built without NUMA support publishes no `/sys/devices/system/node`
/// at all. That is a fact about the kernel, not a failure to read it, and
/// discovery reports it as a topology with no nodes -- `exposed()` is false --
/// rather than either refusing or inventing a single node holding everything.
/// Inventing one would be the plausible wrong answer this module exists to
/// avoid: it would attribute every CPU and all memory to a node the kernel
/// never described.
///
/// WHAT IS CHECKED, AND WHAT DELIBERATELY IS NOT
/// --------------------------------------------
/// Checked, because each is certain of any machine:
///
///   * every CPU a node claims is one the CPU topology found online;
///   * every online CPU belongs to EXACTLY one node -- `cpu_to_node` is a
///     function, so the nodes' CPU lists partition `online`;
///   * a node's distance row has one entry per online node, because the kernel
///     writes it with for_each_online_node;
///   * a node's `meminfo` describes that node: every line is prefixed
///     `Node <id>`, and a file describing a different id is not the file we
///     think it is.
///
/// NOT checked, and the omissions are deliberate rather than oversights:
///
///   * that node memory sums to the machine's memory. FALSE on the VM this was
///     written against -- see NumaNode::memory_bytes.
///   * that a node's distance to itself is 10. The kernel's LOCAL_DISTANCE and
///     the ACPI SLIT convention both say so, and neither is a law of hardware.
///   * that `cpu/cpuN/nodeM` links agree with `nodeM/cpulist`. They do on every
///     kernel seen, and the T8 test asserts it on the real machine -- but as
///     corroboration that can fail a test, not as a rule that can refuse a user.
class NumaTopology {
 public:
  /// Read and cross-check the machine's NUMA layout.
  ///
  /// Takes the CPU topology rather than re-reading `online`, so the two halves
  /// cannot disagree about which CPUs exist.
  [[nodiscard]] static core::Result<NumaTopology, DiscoveryFailure> discover(
      platform::FileSystem& filesystem, const CpuTopology& cpus,
      std::string_view sysfs_root = kDefaultSysfsRoot);

  /// Whether this kernel published a NUMA layout at all.
  [[nodiscard]] bool exposed() const noexcept { return !nodes_.empty(); }

  /// The online nodes, ascending by id. Empty when not exposed.
  [[nodiscard]] const std::vector<NumaNode>& nodes() const noexcept { return nodes_; }

  [[nodiscard]] std::size_t count() const noexcept { return nodes_.size(); }

  /// The node a CPU is local to, or null: when the layout is not exposed, or
  /// when the CPU is not one discovery saw. Null rather than a default node,
  /// for the reason given above -- there is no node to default to.
  [[nodiscard]] const NumaNode* node_of(std::uint32_t cpu) const noexcept;

 private:
  explicit NumaTopology(std::vector<NumaNode> nodes) : nodes_(std::move(nodes)) {}

  std::vector<NumaNode> nodes_;
};

/// Parses a node's `distance` line: online-node distances separated by single
/// spaces, exactly as the kernel writes them.
///
/// Exposed because it is where a format judgement lives.
[[nodiscard]] core::Result<std::vector<std::uint32_t>, std::string_view> parse_distances(
    std::string_view text);

/// Finds `Node <id> MemTotal:` in a node's `meminfo` and returns the value in
/// bytes -- or why it could not.
///
/// The `<id>` is checked against the node the file was read for. The kernel
/// prefixes every line of a node's meminfo with its own id, so a mismatch means
/// the file is not the one we think, and that is worth refusing over.
[[nodiscard]] core::Result<std::uint64_t, std::string_view> parse_node_mem_total(
    const std::vector<std::string>& lines, std::uint32_t id);

}  // namespace loadforge::topology

#endif
