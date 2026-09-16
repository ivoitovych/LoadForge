// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/cpu_topology.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

std::string cpu_directory(std::string_view sysfs_root, std::uint32_t id) {
  return std::string{sysfs_root} + "/devices/system/cpu/cpu" + std::to_string(id);
}

DiscoveryFailure from_source(const Unavailable& unavailable) {
  // Both strings copied into locals first, so the aggregate below constructs
  // from moves alone. Journal §1.13: two allocating members in one initialiser
  // force a cleanup path carrying a branch no test can ever take.
  std::string subject = unavailable.path;
  std::string detail = unavailable.detail;
  return DiscoveryFailure{DiscoveryFailure::Kind::kSource, unavailable.kind, std::move(subject),
                          std::move(detail)};
}

DiscoveryFailure contradiction(std::string subject, std::string detail) {
  return DiscoveryFailure{DiscoveryFailure::Kind::kContradiction, Availability::kPresent,
                          std::move(subject), std::move(detail)};
}

/// Reads one of the small integer ids the kernel writes per CPU.
///
/// A NEGATIVE value is refused, with its own wording, and that case is the
/// reason this helper exists rather than a bare `Source::integer()` call. The
/// kernel writes **-1** into `core_id` and `physical_package_id` when it cannot
/// determine them, which happens on some virtualised and arm64 machines. That
/// is not a malformed file -- it is the kernel saying it does not know -- and a
/// topology built on top of it would be an invention. Saying which is which is
/// the difference between "your kernel wrote something strange" and "your
/// kernel does not expose this", and only the second has an answer the user can
/// act on.
core::Result<std::uint32_t, DiscoveryFailure> read_id(platform::FileSystem& filesystem,
                                                      const std::string& path) {
  Source source(filesystem, path);
  auto value = source.integer();
  if (!value.has_value()) {
    return from_source(value.error());
  }
  if (value.value() < 0) {
    return contradiction(path, "the kernel reports " + std::to_string(value.value()) +
                                   ", meaning it cannot determine this value; no topology can be "
                                   "built on an identity the kernel does not have");
  }
  if (value.value() > CpuList::kMaxCpuId) {
    return contradiction(path, "the kernel reports " + std::to_string(value.value()) +
                                   ", which is past any id this kernel can assign");
  }
  return static_cast<std::uint32_t>(value.value());
}

std::string name_of(std::uint32_t id) { return "cpu" + std::to_string(id); }

}  // namespace

std::string describe(const DiscoveryFailure& failure) {
  if (failure.kind == DiscoveryFailure::Kind::kContradiction) {
    return "the machine describes itself inconsistently at " + failure.subject + ": " +
           failure.detail;
  }
  return failure.subject + ": " + std::string{describe(failure.availability)} + " (" +
         failure.detail + ")";
}

core::Result<CpuTopology, DiscoveryFailure> CpuTopology::discover(platform::FileSystem& filesystem,
                                                                  std::string_view sysfs_root) {
  const std::string online_path = std::string{sysfs_root} + "/devices/system/cpu/online";
  Source online_source(filesystem, online_path);
  auto online = online_source.cpu_list();
  if (!online.has_value()) {
    return from_source(online.error());
  }

  // An empty `online` is a legitimate READING -- CpuList is right to accept it,
  // and `offline` is empty on every healthy machine -- but it is an impossible
  // FACT here. This code is executing, so at least one CPU is online, and a
  // machine claiming otherwise is not the machine we are reading. That is the
  // difference between a parser, which must accept whatever the format allows,
  // and a discovery layer, which knows something about the world the parser
  // does not.
  if (online.value().empty()) {
    return contradiction(online_path, "no CPU is reported online, yet this code is running on one");
  }

  std::vector<LogicalCpu> cpus;
  cpus.reserve(online.value().ids().size());

  for (const std::uint32_t id : online.value().ids()) {
    const std::string directory = cpu_directory(sysfs_root, id);

    auto package = read_id(filesystem, directory + "/topology/physical_package_id");
    if (!package.has_value()) {
      return package.error();
    }
    auto core = read_id(filesystem, directory + "/topology/core_id");
    if (!core.has_value()) {
      return core.error();
    }

    const std::string siblings_path = directory + "/topology/thread_siblings_list";
    Source siblings_source(filesystem, siblings_path);
    auto siblings = siblings_source.cpu_list();
    if (!siblings.has_value()) {
      return from_source(siblings.error());
    }

    // A CPU that is not among its own thread siblings is the cheapest possible
    // sign that this file is not what we think it is -- and it is checked
    // first because every later check reasons FROM the sibling sets, so one
    // that does not contain its own CPU would silently poison them.
    if (!siblings.value().contains(id)) {
      return contradiction(siblings_path, name_of(id) + " is not among its own thread siblings");
    }

    cpus.push_back(LogicalCpu{id, CoreAddress{package.value(), core.value()}, siblings.value()});
  }

  // The cross-checks that need every CPU read before any of them can be made.
  // Kept in a second pass rather than folded into the loop above, because a
  // check on a pair is not answerable until both halves exist, and half-checked
  // pairs are how an inconsistency slips through.
  for (const LogicalCpu& cpu : cpus) {
    for (const std::uint32_t sibling_id : cpu.thread_siblings.ids()) {
      const auto sibling =
          std::find_if(cpus.begin(), cpus.end(),
                       [sibling_id](const LogicalCpu& c) { return c.id == sibling_id; });

      // A sibling that is not in the list is a sibling that is not online.
      // Refused rather than skipped: the scheduler's masks and the hotplug
      // machinery are describing different machines, and quietly dropping the
      // missing CPU would produce a core that looks smaller than it is.
      if (sibling == cpus.end()) {
        return contradiction(cpu_directory(sysfs_root, cpu.id) + "/topology/thread_siblings_list",
                             name_of(cpu.id) + " lists " + name_of(sibling_id) +
                                 " as a thread sibling, but " + name_of(sibling_id) +
                                 " is not online");
      }

      // Sibling sets are equivalence classes: sharing a physical core is
      // symmetric and transitive, so two CPUs on one core must publish the
      // SAME set, not merely mention each other. Checking only for mutual
      // mention would accept "0 lists 0,1; 1 lists 0,1,2; 2 lists 2", which
      // describes no arrangement of hardware.
      if (sibling->thread_siblings != cpu.thread_siblings) {
        return contradiction(cpu_directory(sysfs_root, cpu.id) + "/topology/thread_siblings_list",
                             name_of(cpu.id) + " and " + name_of(sibling_id) +
                                 " claim to share a core but publish different sibling sets");
      }

      // Same core means same core ADDRESS. If these disagree, either the
      // sibling masks or the per-CPU ids are wrong, and there is no way to tell
      // which -- so neither is trusted.
      if (sibling->core != cpu.core) {
        return contradiction(cpu_directory(sysfs_root, cpu.id) + "/topology",
                             name_of(cpu.id) + " and " + name_of(sibling_id) +
                                 " share a core according to their sibling sets, but report "
                                 "different core or package ids");
      }
    }
  }

  return CpuTopology{std::move(cpus)};
}

std::size_t CpuTopology::physical_core_count() const noexcept {
  std::vector<std::uint64_t> cores;
  cores.reserve(cpus_.size());
  for (const LogicalCpu& cpu : cpus_) {
    cores.push_back(key(cpu.core));
  }
  std::sort(cores.begin(), cores.end());
  return static_cast<std::size_t>(
      std::distance(cores.begin(), std::unique(cores.begin(), cores.end())));
}

std::size_t CpuTopology::package_count() const noexcept {
  std::vector<std::uint32_t> packages;
  packages.reserve(cpus_.size());
  for (const LogicalCpu& cpu : cpus_) {
    packages.push_back(cpu.core.package);
  }
  std::sort(packages.begin(), packages.end());
  return static_cast<std::size_t>(
      std::distance(packages.begin(), std::unique(packages.begin(), packages.end())));
}

bool CpuTopology::multithreaded() const noexcept {
  return std::any_of(cpus_.begin(), cpus_.end(),
                     [](const LogicalCpu& cpu) { return cpu.thread_siblings.size() > 1; });
}

}  // namespace loadforge::topology
