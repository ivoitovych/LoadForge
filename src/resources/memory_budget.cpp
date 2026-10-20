// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources/memory_budget.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "core/result.hpp"
#include "platform/fs.hpp"
#include "resources/cgroup.hpp"
#include "resources/decimal.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::resources {
namespace {

constexpr std::string_view kMeminfoFile = "/proc/meminfo";
constexpr std::string_view kAvailableKey = "MemAvailable:";
constexpr std::string_view kKilobytes = " kB";
constexpr std::uint64_t kBytesPerKilobyte = 1024;
constexpr std::string_view kHierarchicalKey = "hierarchical_memory_limit ";

/// v1: the kernel's own effective limit, from the cgroup's memory.stat.
core::Result<std::optional<std::uint64_t>, topology::DiscoveryFailure> v1_limit(
    platform::FileSystem& filesystem, const MemoryCgroup& cgroup) {
  const std::string file = directory_of(cgroup) + "/memory.stat";
  topology::Source source(filesystem, file);
  auto lines = source.lines();
  if (!lines.has_value()) {
    // Absent included: the directory carries memory.limit_in_bytes (the probe
    // that located it read it), and every v1 memory cgroup carries both.
    return topology::from_source(lines.error());
  }
  auto text = hierarchical_limit_text(lines.value());
  if (!text.has_value()) {
    std::string detail{text.error()};
    return source_failure(topology::Availability::kMalformed, file, std::move(detail));
  }
  auto limit = parse_memory_limit(text.value(), CgroupVersion::kV1);
  if (!limit.has_value()) {
    std::string detail = limit.error();
    return source_failure(topology::Availability::kMalformed, file, std::move(detail));
  }
  return limit.value();
}

/// The lowest of the limits read so far and the one just read.
std::optional<std::uint64_t> lower(std::optional<std::uint64_t> so_far,
                                   std::optional<std::uint64_t> here) {
  if (!here.has_value()) {
    return so_far;
  }
  if (!so_far.has_value()) {
    return here;
  }
  return std::min(*so_far, *here);
}

}  // namespace

core::Result<std::optional<std::uint64_t>, std::string> parse_mem_available(
    const std::vector<std::string>& lines) {
  std::optional<std::uint64_t> found;
  for (const std::string& line : lines) {
    std::string_view rest{line};
    if (!rest.starts_with(kAvailableKey)) {
      continue;
    }
    if (found.has_value()) {
      return std::string{"MemAvailable appears twice"};
    }
    rest.remove_prefix(kAvailableKey.size());
    // The kernel pads the value to a column with spaces; how many depends on
    // the key's length and is not part of the format.
    while (rest.starts_with(' ')) {
      rest.remove_prefix(1);
    }
    if (!rest.ends_with(kKilobytes)) {
      return "\"" + line + "\" is not in kB";
    }
    rest.remove_suffix(kKilobytes.size());
    const std::optional<std::uint64_t> kilobytes = parse_decimal(rest);
    if (!kilobytes.has_value() ||
        *kilobytes > std::numeric_limits<std::uint64_t>::max() / kBytesPerKilobyte) {
      return "\"" + line + "\" does not hold a number of kB a byte count can carry";
    }
    found = *kilobytes * kBytesPerKilobyte;
  }
  return found;
}

core::Result<std::optional<std::uint64_t>, std::string> parse_memory_limit(std::string_view text,
                                                                           CgroupVersion version) {
  if (version == CgroupVersion::kV2 && text == "max") {
    return std::optional<std::uint64_t>{};
  }
  const std::optional<std::uint64_t> bytes = parse_decimal(text);
  if (!bytes.has_value() || *bytes > kLargestLimit) {
    std::string reason = "\"" + std::string{text} + "\" is not a limit ";
    reason += describe(version);
    reason += " writes";
    return reason;
  }
  if (version == CgroupVersion::kV1 && *bytes >= kV1UnlimitedFloor) {
    return std::optional<std::uint64_t>{};
  }
  return bytes;
}

core::Result<std::string, std::string_view> hierarchical_limit_text(
    const std::vector<std::string>& lines) {
  std::optional<std::string> found;
  for (const std::string& line : lines) {
    if (!line.starts_with(kHierarchicalKey)) {
      continue;
    }
    if (found.has_value()) {
      return std::string_view{"hierarchical_memory_limit appears twice"};
    }
    found = line.substr(kHierarchicalKey.size());
  }
  if (!found.has_value()) {
    return std::string_view{"no hierarchical_memory_limit line"};
  }
  return *found;
}

core::Result<config::MemoryBudget, topology::DiscoveryFailure> read_memory_budget(
    platform::FileSystem& filesystem, std::string_view root) {
  const std::string meminfo_file = std::string{root} + std::string{kMeminfoFile};
  topology::Source meminfo(filesystem, meminfo_file);
  auto lines = meminfo.lines();
  if (!lines.has_value()) {
    return topology::from_source(lines.error());
  }
  auto available = parse_mem_available(lines.value());
  if (!available.has_value()) {
    std::string detail = available.error();
    return source_failure(topology::Availability::kMalformed, meminfo_file, std::move(detail));
  }
  const std::optional<std::uint64_t>& available_bytes = available.value();
  if (!available_bytes.has_value()) {
    std::string detail =
        "no MemAvailable line: the kernel's own estimate arrived in Linux 3.14, and this reader "
        "does not make one up";
    return source_failure(topology::Availability::kAbsent, meminfo_file, std::move(detail));
  }

  auto located = locate_memory_cgroup(filesystem, root);
  if (!located.has_value()) {
    return located.error();
  }
  config::MemoryBudget budget;
  budget.available_bytes = *available_bytes;
  const std::optional<MemoryCgroup>& found = located.value();
  if (!found.has_value()) {
    return budget;  // In no memory cgroup: nothing limits this process through one.
  }
  const MemoryCgroup& cgroup = *found;

  if (cgroup.version == CgroupVersion::kV1) {
    auto limit = v1_limit(filesystem, cgroup);
    if (!limit.has_value()) {
      return limit.error();
    }
    budget.cgroup_limit_bytes = limit.value();
    return budget;
  }

  std::optional<std::uint64_t> limit;
  for (const std::string& directory : directories_upward(cgroup)) {
    const std::string file = directory + "/memory.max";
    topology::Source source(filesystem, file);
    auto text = source.text();
    if (!text.has_value()) {
      // No memory.max is a reading: the hierarchy's root has none, and
      // neither has a cgroup whose parent does not enable the memory
      // controller for it. No limit is set at this level.
      if (text.error().kind == topology::Availability::kAbsent) {
        continue;
      }
      return topology::from_source(text.error());
    }
    auto parsed = parse_memory_limit(text.value(), CgroupVersion::kV2);
    if (!parsed.has_value()) {
      std::string detail = parsed.error();
      return source_failure(topology::Availability::kMalformed, file, std::move(detail));
    }
    limit = lower(limit, parsed.value());
  }
  budget.cgroup_limit_bytes = limit;
  return budget;
}

}  // namespace loadforge::resources
