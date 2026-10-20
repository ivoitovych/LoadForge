// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_RESOURCES_CGROUP_HPP
#define LOADFORGE_RESOURCES_CGROUP_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::resources {

/// Where the kernel's files hang from. Empty on the running machine, so a path
/// is "/proc/meminfo" exactly; a test passes a directory it built, and every
/// path this module reads -- /proc and the cgroup mounts alike -- moves with it.
inline constexpr std::string_view kMachineRoot{};

/// Which cgroup hierarchy holds this process's memory controller.
///
/// Both are real on machines this project runs on (journal §1.18): v2 on a
/// current distribution, v1 in the container this was written in and on every
/// host still booted with the legacy layout. The two publish different files
/// for the same facts, so every reader above says which one it found.
enum class CgroupVersion : std::uint8_t { kV1, kV2 };

/// "cgroup v1" or "cgroup v2".
[[nodiscard]] std::string_view describe(CgroupVersion version) noexcept;

/// The memory cgroup /proc/self/cgroup places this process in.
///
/// `path` is as the kernel prints it: absolute within the hierarchy as this
/// process's cgroup namespace sees it, "/" for the hierarchy's root.
struct CgroupMembership {
  CgroupVersion version = CgroupVersion::kV2;
  std::string path;
};

/// Reads the lines of /proc/self/cgroup.
///
/// Each line is "hierarchy-id:controllers:path". A v1 hierarchy that lists
/// `memory` among its controllers holds the memory controller, and its line
/// wins: on a hybrid machine the unified hierarchy is mounted beside the v1
/// ones and has no memory files at all. Otherwise the unified line "0::path"
/// is the answer, and whether the memory controller is enabled there is for
/// the files under it to say.
///
/// Nothing when neither exists: a kernel that predates cgroup v2 and has no v1
/// memory hierarchy has no memory cgroup, and nothing can limit this process
/// through one. Refused, with the reason, when a line is not in the kernel's
/// format, or when two lines claim the same thing.
[[nodiscard]] core::Result<std::optional<CgroupMembership>, std::string> parse_membership(
    const std::vector<std::string>& lines);

/// The fields of one /proc/self/mountinfo line this module uses.
///
/// `root` and `mount_point` are unescaped: the kernel writes a space in a path
/// as "\040", and a path compared or opened in its escaped form names nothing.
struct MountEntry {
  std::string root;         ///< The directory of the mounted filesystem that this mount shows.
  std::string mount_point;  ///< Where that directory appears.
  std::string fstype;       ///< "cgroup" for a v1 hierarchy, "cgroup2" for the unified one.
  std::string super_options;
};

/// Parses one line of /proc/self/mountinfo (proc(5)): ten or more fields,
/// with a run of optional fields ended by a lone "-" before the filesystem
/// type, source and super options. Refused, with the reason, when the line
/// does not have that shape.
[[nodiscard]] core::Result<MountEntry, std::string> parse_mount_entry(std::string_view line);

/// The memory cgroup, found on disk.
///
/// `mount` is the directory the hierarchy is mounted at, prefixed with the
/// machine root; `relative` is the cgroup's path below that mount's own root,
/// empty when the cgroup IS that root. Kept apart because the walk up the
/// hierarchy ends at the mount: what lies above a mount's root is not visible
/// from this mount namespace, and a container is usually given exactly its own
/// subtree.
struct MemoryCgroup {
  CgroupVersion version = CgroupVersion::kV2;
  std::string mount;
  std::string relative;
};

/// The cgroup's own directory.
[[nodiscard]] std::string directory_of(const MemoryCgroup& cgroup);

/// The cgroup's directory and each ancestor's up to the mount, own first. A
/// limit set on any of them binds the process, so a reader of the limit reads
/// them all.
[[nodiscard]] std::vector<std::string> directories_upward(const MemoryCgroup& cgroup);

/// A source that would not answer, as discovery reports it: which source, in
/// which capability state, and why. Every reader here reports through it.
///
/// `detail` is taken by value and moved, and `subject` by reference and
/// copied inside, so a caller passes a local it built on its own line and the
/// failure is assembled with one allocation and no temporary around a call
/// that might throw. Journal §1.13: a temporary with two strings in it, handed
/// to a function, leaves a cleanup edge no test can take.
[[nodiscard]] topology::DiscoveryFailure source_failure(topology::Availability availability,
                                                        const std::string& subject,
                                                        std::string detail);

/// Finds this process's memory cgroup: /proc/self/cgroup says which, and
/// /proc/self/mountinfo says where its hierarchy is mounted.
///
/// WHY MOUNTINFO, AND NOT /sys/fs/cgroup
/// -------------------------------------
/// The conventional mount points are right on a host and wrong in the case
/// that matters most. A container on a v1 host without a cgroup namespace
/// sees /proc/self/cgroup name "/docker/<id>" while its memory hierarchy is
/// mounted with that directory as the mount's ROOT: the files are at
/// /sys/fs/cgroup/memory/, and /sys/fs/cgroup/memory/docker/<id>/ does not
/// exist. A reader that joined the two would find nothing, call it "no limit",
/// and size the run against the host's memory inside a container with a wall
/// much nearer. The mount's root is in mountinfo, and only there.
///
/// Nothing when /proc/self/cgroup is absent (a kernel without cgroups) or
/// names no memory cgroup. A failure, never a guess, when the hierarchy is
/// not mounted where this process can see it, when the cgroup lies outside
/// every mount of it, or when its directory is not there: each of those means
/// the limit exists and cannot be read, which is not the same as no limit.
[[nodiscard]] core::Result<std::optional<MemoryCgroup>, topology::DiscoveryFailure>
locate_memory_cgroup(platform::FileSystem& filesystem, std::string_view root);

}  // namespace loadforge::resources

#endif
