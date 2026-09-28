// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/numa.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

/// Largest MemTotal accepted, in kB. 1 PiB of memory on one node is not a node;
/// it is a file that is not what we think it is.
constexpr std::uint64_t kMaxMemoryKb = 1ULL << 40U;

/// Largest distance accepted. ACPI encodes them in a byte; device trees in a
/// u32 that no platform fills. Past this the line is not a distance row.
constexpr std::uint64_t kMaxDistance = 65535;

std::string node_directory(std::string_view sysfs_root, std::uint32_t id) {
  return std::string{sysfs_root} + "/devices/system/node/node" + std::to_string(id);
}

std::string name_of(std::uint32_t cpu) { return "cpu" + std::to_string(cpu); }
std::string node_name(std::uint32_t id) { return "node" + std::to_string(id); }

/// Digits only, bounded before the multiply (journal §4.6). Returns the value
/// consumed and how many characters it spanned, or nothing if the text does
/// not begin with a digit.
struct Digits {
  std::uint64_t value = 0;
  std::size_t length = 0;
};

std::optional<Digits> read_digits(std::string_view text, std::uint64_t bound) {
  Digits digits;
  while (digits.length < text.size() && text[digits.length] >= '0' && text[digits.length] <= '9') {
    const auto digit = static_cast<std::uint64_t>(text[digits.length] - '0');
    if (digits.value > (bound - digit) / 10) {
      return std::nullopt;  // Past the bound: to the caller, not a number.
    }
    digits.value = digits.value * 10 + digit;
    ++digits.length;
  }
  if (digits.length == 0) {
    return std::nullopt;
  }
  return digits;
}

/// Reads one node's three attributes and assembles it. Cross-checks that need
/// every node come afterwards, in discover().
core::Result<NumaNode, DiscoveryFailure> read_node(platform::FileSystem& filesystem,
                                                   std::string_view sysfs_root, std::uint32_t id,
                                                   std::size_t online_node_count) {
  const std::string directory = node_directory(sysfs_root, id);

  Source cpus_source(filesystem, directory + "/cpulist");
  auto cpus = cpus_source.cpu_list();
  if (!cpus.has_value()) {
    return from_source(cpus.error());
  }

  Source meminfo_source(filesystem, directory + "/meminfo");
  auto lines = meminfo_source.lines();
  if (!lines.has_value()) {
    return from_source(lines.error());
  }
  auto memory_bytes = parse_node_mem_total(lines.value(), id);
  if (!memory_bytes.has_value()) {
    std::string detail{memory_bytes.error()};
    return contradiction(directory + "/meminfo", std::move(detail));
  }

  Source distance_source(filesystem, directory + "/distance");
  auto distance_text = distance_source.text();
  if (!distance_text.has_value()) {
    return from_source(distance_text.error());
  }
  auto distances = parse_distances(distance_text.value());
  if (!distances.has_value()) {
    std::string detail = "\"" + distance_text.value() + "\": " + std::string{distances.error()};
    return contradiction(directory + "/distance", std::move(detail));
  }
  // The kernel writes one entry per online node. A row of the wrong length
  // means the distance table and the online set describe different machines.
  if (distances.value().size() != online_node_count) {
    std::string detail = node_name(id) + " reports " + std::to_string(distances.value().size()) +
                         " distance(s) but " + std::to_string(online_node_count) +
                         " node(s) are online";
    return contradiction(directory + "/distance", std::move(detail));
  }

  NumaNode node;
  node.id = id;
  node.cpus = cpus.value();
  node.memory_bytes = memory_bytes.value();
  node.distances = distances.value();
  return node;
}

/// Every CPU a node claims is one the CPU topology found online, and every
/// online CPU is claimed by exactly one node. Both halves of a partition, and
/// both certain: cpu_to_node is a function.
std::optional<DiscoveryFailure> check_partition(const std::vector<NumaNode>& nodes,
                                                const CpuTopology& cpus,
                                                std::string_view sysfs_root) {
  for (const NumaNode& node : nodes) {
    for (const std::uint32_t claimed : node.cpus.ids()) {
      const bool online = std::any_of(cpus.cpus().begin(), cpus.cpus().end(),
                                      [claimed](const LogicalCpu& c) { return c.id == claimed; });
      if (!online) {
        std::string detail =
            node_name(node.id) + " claims " + name_of(claimed) + ", which is not online";
        return contradiction(node_directory(sysfs_root, node.id) + "/cpulist", std::move(detail));
      }
    }
  }
  for (const LogicalCpu& cpu : cpus.cpus()) {
    std::size_t owners = 0;
    for (const NumaNode& node : nodes) {
      if (node.cpus.contains(cpu.id)) {
        ++owners;
      }
    }
    if (owners != 1) {
      // An if rather than a ternary, and not for taste: a ternary whose arms
      // are a literal and an allocating std::string builds both inside one
      // expression, and the cleanup paths that come with that carry branches
      // no test can take (journal §1.13). Same message, one allocation at a
      // time.
      std::string reason = " belongs to no node";
      if (owners != 0) {
        reason = " is claimed by " + std::to_string(owners) + " nodes";
      }
      std::string detail = name_of(cpu.id) + reason;
      return contradiction(std::string{sysfs_root} + "/devices/system/node", std::move(detail));
    }
  }
  return std::nullopt;
}

}  // namespace

core::Result<std::vector<std::uint32_t>, std::string_view> parse_distances(std::string_view text) {
  if (text.empty()) {
    return std::string_view{"a distance row is never written blank"};
  }
  std::vector<std::uint32_t> distances;
  std::size_t position = 0;
  // `while (true)`, as in CpuList::parse: the only exit is running out of
  // text after a number, and a loop condition that can never be false reads as
  // a check nobody has to think about.
  while (true) {
    const auto digits = read_digits(text.substr(position), kMaxDistance);
    if (!digits.has_value()) {
      return std::string_view{"expected a distance, found something that is not one"};
    }
    distances.push_back(static_cast<std::uint32_t>(digits->value));
    position += digits->length;
    if (position == text.size()) {
      return distances;
    }
    // Exactly one space, exactly as the kernel writes it. A double space or a
    // comma would parse under a lenient split and mean the file is not what we
    // think.
    if (text[position] != ' ') {
      return std::string_view{"distances are separated by single spaces"};
    }
    ++position;
    if (position == text.size()) {
      return std::string_view{"a distance row does not end in a space"};
    }
  }
}

core::Result<std::uint64_t, std::string_view> parse_node_mem_total(
    const std::vector<std::string>& lines, std::uint32_t id) {
  constexpr std::string_view kPrefix = "Node ";
  constexpr std::string_view kField = "MemTotal:";
  constexpr std::string_view kUnit = " kB";

  for (const std::string& line : lines) {
    const std::string_view view{line};
    const std::size_t field = view.find(kField);
    if (field == std::string_view::npos) {
      continue;
    }
    // Found the line. Everything from here is a claim about its shape, and
    // each is refused on its own terms rather than by falling through.
    if (!view.starts_with(kPrefix)) {
      return std::string_view{"the MemTotal line does not begin with \"Node \""};
    }
    const auto line_id = read_digits(view.substr(kPrefix.size()), CpuList::kMaxCpuId);
    // "Node <id> MemTotal:" exactly: the digits run to one space before the
    // field, and that space is the whole separator.
    if (!line_id.has_value() || kPrefix.size() + line_id->length + 1 != field ||
        view[field - 1] != ' ') {
      return std::string_view{"the MemTotal line does not name a node"};
    }
    if (line_id->value != id) {
      return std::string_view{"meminfo describes a different node than the one it was read for"};
    }
    std::string_view rest = view.substr(field + kField.size());
    while (rest.starts_with(' ')) {
      rest.remove_prefix(1);
    }
    const auto value = read_digits(rest, kMaxMemoryKb);
    if (!value.has_value()) {
      return std::string_view{"MemTotal is not a number, or is larger than any machine"};
    }
    if (rest.substr(value->length) != kUnit) {
      return std::string_view{"MemTotal is not in kB"};
    }
    return value->value * 1024;
  }
  return std::string_view{"meminfo has no MemTotal line"};
}

core::Result<NumaTopology, DiscoveryFailure> NumaTopology::discover(
    platform::FileSystem& filesystem, const CpuTopology& cpus, std::string_view sysfs_root) {
  const std::string online_path = std::string{sysfs_root} + "/devices/system/node/online";
  Source online_source(filesystem, online_path);
  // The same format as a CPU list -- the kernel writes both with the same
  // formatter -- so the same strict parser reads it.
  auto online = online_source.cpu_list();
  if (!online.has_value()) {
    // Absent is the one failure that is an answer: this kernel publishes no
    // NUMA layout. Every other failure is still a failure.
    if (online.error().kind == Availability::kAbsent) {
      return NumaTopology{{}};
    }
    return from_source(online.error());
  }
  // A published layout with no online node is impossible for the same reason
  // an empty `cpu/online` is: this code is running, on a CPU, on a node.
  if (online.value().empty()) {
    return contradiction(online_path,
                         "no node is reported online, yet this code is running on one");
  }

  std::vector<NumaNode> nodes;
  nodes.reserve(online.value().size());
  for (const std::uint32_t id : online.value().ids()) {
    auto node = read_node(filesystem, sysfs_root, id, online.value().size());
    if (!node.has_value()) {
      return node.error();
    }
    nodes.push_back(node.value());
  }

  if (auto failure = check_partition(nodes, cpus, sysfs_root)) {
    return *failure;
  }
  return NumaTopology{std::move(nodes)};
}

const NumaNode* NumaTopology::node_of(std::uint32_t cpu) const noexcept {
  for (const NumaNode& node : nodes_) {
    if (node.cpus.contains(cpu)) {
      return &node;
    }
  }
  return nullptr;
}

}  // namespace loadforge::topology
