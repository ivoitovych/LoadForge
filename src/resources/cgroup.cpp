// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources/cgroup.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "resources/decimal.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::resources {
namespace {

constexpr std::string_view kMembershipFile = "/proc/self/cgroup";
constexpr std::string_view kMountinfoFile = "/proc/self/mountinfo";

/// Whether a comma-separated controller list names `memory` as a whole word.
/// A substring test would match a controller called "memory_tiering" and
/// place the process in the wrong hierarchy.
bool names_memory(std::string_view controllers) {
  while (true) {
    const std::size_t comma = controllers.find(',');
    if (controllers.substr(0, comma) == "memory") {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    controllers.remove_prefix(comma + 1);
  }
}

/// "\ooo" back to the byte it stands for, everywhere in a mountinfo path.
/// The kernel escapes a space, a tab, a newline and a backslash this way, and
/// nothing else; a backslash not followed by three octal digits is not
/// something it writes.
std::optional<std::string> unescape(std::string_view field) {
  std::string out;
  out.reserve(field.size());
  for (std::size_t i = 0; i < field.size(); ++i) {
    if (field[i] != '\\') {
      out.push_back(field[i]);
      continue;
    }
    if (i + 3 >= field.size()) {
      return std::nullopt;
    }
    unsigned value = 0;
    for (std::size_t k = 1; k <= 3; ++k) {
      const char digit = field[i + k];
      if (digit < '0' || digit > '7') {
        return std::nullopt;
      }
      value = value * 8 + static_cast<unsigned>(digit - '0');
    }
    if (value > 0377U) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(value));
    i += 3;
  }
  return out;
}

/// Splits on single spaces. An empty field -- two spaces together, or one at
/// either end -- is not something mountinfo contains, so it is reported as
/// nothing rather than as an empty field the caller would misnumber.
std::optional<std::vector<std::string_view>> fields_of(std::string_view line) {
  std::vector<std::string_view> fields;
  while (true) {
    const std::size_t space = line.find(' ');
    const std::string_view field = line.substr(0, space);
    if (field.empty()) {
      return std::nullopt;
    }
    fields.push_back(field);
    if (space == std::string_view::npos) {
      return fields;
    }
    line.remove_prefix(space + 1);
  }
}

/// The cgroup's path below a mount whose root is `mount_root`, or nothing when
/// the cgroup is not under it. Compared by whole path components: a mount of
/// "/docker/ab" does not show "/docker/abc".
std::optional<std::string> below(std::string_view mount_root, std::string_view path) {
  if (mount_root == "/") {
    return std::string{path == "/" ? std::string_view{} : path};
  }
  if (path == mount_root) {
    return std::string{};
  }
  if (path.starts_with(mount_root) && path[mount_root.size()] == '/') {
    return std::string{path.substr(mount_root.size())};
  }
  return std::nullopt;
}

bool is_hierarchy_mount(const MountEntry& entry, CgroupVersion version) {
  if (version == CgroupVersion::kV2) {
    return entry.fstype == "cgroup2";
  }
  return entry.fstype == "cgroup" && names_memory(entry.super_options);
}

/// The file whose presence shows a cgroup's directory is really there.
/// `cgroup.controllers` is in every v2 cgroup, the root included; v1 has no
/// equivalent common to every hierarchy, but every directory of the memory
/// hierarchy carries `memory.limit_in_bytes`, the root included.
std::string_view probe_file(CgroupVersion version) {
  return version == CgroupVersion::kV2 ? "cgroup.controllers" : "memory.limit_in_bytes";
}

}  // namespace

std::string_view describe(CgroupVersion version) noexcept {
  switch (version) {
    case CgroupVersion::kV1:
      return "cgroup v1";
    case CgroupVersion::kV2:
      return "cgroup v2";
  }
  return "unrecognised cgroup version";
}

core::Result<std::optional<CgroupMembership>, std::string> parse_membership(
    const std::vector<std::string>& lines) {
  std::optional<std::string> memory_path;
  std::optional<std::string> unified_path;
  for (const std::string& line : lines) {
    const std::size_t first = line.find(':');
    const std::size_t second =
        first == std::string::npos ? std::string::npos : line.find(':', first + 1);
    if (second == std::string::npos) {
      return "\"" + line + "\" is not hierarchy-id:controllers:path";
    }
    const std::optional<std::uint64_t> id = parse_decimal(std::string_view{line}.substr(0, first));
    if (!id.has_value()) {
      return "\"" + line + "\" does not begin with a hierarchy id";
    }
    const std::string_view controllers =
        std::string_view{line}.substr(first + 1, second - first - 1);
    std::string path = line.substr(second + 1);
    if (!path.starts_with('/')) {
      return "\"" + line + "\" does not end in an absolute cgroup path";
    }
    if (*id == 0) {
      if (!controllers.empty()) {
        return "\"" + line + "\" lists controllers on the unified hierarchy, which has none here";
      }
      if (unified_path.has_value()) {
        return std::string{"the unified hierarchy is listed twice"};
      }
      unified_path = std::move(path);
    } else if (names_memory(controllers)) {
      if (memory_path.has_value()) {
        return std::string{"two hierarchies both claim the memory controller"};
      }
      memory_path = std::move(path);
    }
  }
  if (memory_path.has_value()) {
    return std::optional<CgroupMembership>{CgroupMembership{CgroupVersion::kV1, *memory_path}};
  }
  if (unified_path.has_value()) {
    return std::optional<CgroupMembership>{CgroupMembership{CgroupVersion::kV2, *unified_path}};
  }
  return std::optional<CgroupMembership>{};
}

core::Result<MountEntry, std::string> parse_mount_entry(std::string_view line) {
  const auto fields = fields_of(line);
  if (!fields.has_value()) {
    return std::string{"fields are separated by single spaces"};
  }
  const std::vector<std::string_view>& f = fields.value();
  // Six fixed fields, then optional ones, then "-" and exactly three more.
  constexpr std::size_t kFirstOptional = 6;
  constexpr std::size_t kAfterSeparator = 3;
  std::size_t separator = kFirstOptional;
  while (separator < f.size() && f[separator] != "-") {
    ++separator;
  }
  if (separator == f.size() || separator + kAfterSeparator + 1 != f.size()) {
    return std::string{"no \"-\" followed by exactly a type, a source and options"};
  }
  auto root = unescape(f[3]);
  auto mount_point = unescape(f[4]);
  if (!root.has_value() || !mount_point.has_value()) {
    return std::string{"a path carries an escape the kernel does not write"};
  }
  MountEntry entry;
  entry.root = std::move(*root);
  entry.mount_point = std::move(*mount_point);
  entry.fstype = std::string{f[separator + 1]};
  entry.super_options = std::string{f[separator + 3]};
  return entry;
}

topology::DiscoveryFailure source_failure(topology::Availability availability,
                                          const std::string& subject, std::string detail) {
  return topology::DiscoveryFailure{topology::DiscoveryFailure::Kind::kSource, availability,
                                    subject, std::move(detail)};
}

std::string directory_of(const MemoryCgroup& cgroup) { return cgroup.mount + cgroup.relative; }

std::vector<std::string> directories_upward(const MemoryCgroup& cgroup) {
  std::vector<std::string> directories;
  std::string_view relative = cgroup.relative;
  while (true) {
    directories.push_back(cgroup.mount + std::string{relative});
    if (relative.empty()) {
      return directories;
    }
    relative = relative.substr(0, relative.rfind('/'));
  }
}

core::Result<std::optional<MemoryCgroup>, topology::DiscoveryFailure> locate_memory_cgroup(
    platform::FileSystem& filesystem, std::string_view root) {
  const std::string membership_file = std::string{root} + std::string{kMembershipFile};
  topology::Source membership_source(filesystem, membership_file);
  auto membership_lines = membership_source.lines();
  if (!membership_lines.has_value()) {
    // Absent is the one failure that is an answer: a kernel built without
    // cgroups has no /proc/self/cgroup and no cgroup can limit anything.
    if (membership_lines.error().kind == topology::Availability::kAbsent) {
      return std::optional<MemoryCgroup>{};
    }
    return topology::from_source(membership_lines.error());
  }
  auto membership = parse_membership(membership_lines.value());
  if (!membership.has_value()) {
    std::string detail = membership.error();
    return source_failure(topology::Availability::kMalformed, membership_file, std::move(detail));
  }
  // Bound once and checked once (topology/cache.cpp says why: clang-tidy
  // cannot see that two calls to value() return the same optional).
  const std::optional<CgroupMembership>& named = membership.value();
  if (!named.has_value()) {
    return std::optional<MemoryCgroup>{};
  }
  const CgroupMembership& member = *named;

  const std::string mountinfo_file = std::string{root} + std::string{kMountinfoFile};
  topology::Source mountinfo_source(filesystem, mountinfo_file);
  auto mount_lines = mountinfo_source.lines();
  if (!mount_lines.has_value()) {
    return topology::from_source(mount_lines.error());
  }
  bool hierarchy_mounted = false;
  std::optional<MemoryCgroup> found;
  for (const std::string& line : mount_lines.value()) {
    auto entry = parse_mount_entry(line);
    if (!entry.has_value()) {
      std::string detail = "\"" + line + "\": " + entry.error();
      return source_failure(topology::Availability::kMalformed, mountinfo_file, std::move(detail));
    }
    if (found.has_value() || !is_hierarchy_mount(entry.value(), member.version)) {
      continue;
    }
    hierarchy_mounted = true;
    auto relative = below(entry.value().root, member.path);
    if (relative.has_value()) {
      found = MemoryCgroup{member.version, std::string{root} + entry.value().mount_point,
                           std::move(*relative)};
    }
  }
  if (!found.has_value()) {
    std::string detail = "/proc/self/cgroup places this process in " + member.path + " of the " +
                         std::string{describe(member.version)} + " memory hierarchy, but ";
    if (hierarchy_mounted) {
      detail += "no mount of that hierarchy shows it";
    } else {
      detail += "that hierarchy is not mounted where this process can see it";
    }
    return topology::contradiction(mountinfo_file, std::move(detail));
  }

  // The two files agree on a directory; whether it is there is the third
  // source's to say. Absent here is a contradiction, never "no limit": the
  // cgroup this process is in cannot be missing while the process is in it.
  const std::string probe = directory_of(*found) + "/" + std::string{probe_file(found->version)};
  topology::Source probe_source(filesystem, probe);
  auto probed = probe_source.text();
  if (!probed.has_value()) {
    if (probed.error().kind == topology::Availability::kAbsent) {
      return topology::contradiction(probe,
                                     "/proc/self/cgroup and /proc/self/mountinfo place this "
                                     "process's memory cgroup here, and it is not there");
    }
    return topology::from_source(probed.error());
  }
  return found;
}

}  // namespace loadforge::resources
