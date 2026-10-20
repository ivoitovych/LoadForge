// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_RESOURCES_MEMORY_BUDGET_HPP
#define LOADFORGE_RESOURCES_MEMORY_BUDGET_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/result.hpp"
#include "platform/fs.hpp"
#include "resources/cgroup.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::resources {

/// The smallest number a v1 `memory.limit_in_bytes` holds to mean "no limit".
///
/// The kernel has no word for unlimited in v1; it prints its page counter's
/// maximum in bytes, LONG_MAX rounded down to a whole page: 9223372036854771712
/// with 4 KiB pages (read on the machine this was written on, after writing -1
/// to a fresh cgroup), a different number with 16 or 64 KiB pages. Every one of
/// them lies within the largest page size Linux supports (256 KiB) of LONG_MAX,
/// so everything from here up is the sentinel on some page size, and a real
/// limit up there would be eight exbibytes -- no limit, to any machine.
inline constexpr std::uint64_t kV1UnlimitedFloor =
    9'223'372'036'854'775'807ULL - (256ULL << 10U) + 1;

/// The largest byte count a cgroup limit can hold: the page counter is a
/// signed long. A file showing more is not one the kernel wrote.
inline constexpr std::uint64_t kLargestLimit = 9'223'372'036'854'775'807ULL;

/// MemAvailable, in bytes, from the lines of /proc/meminfo.
///
/// MemAvailable and not MemFree or MemTotal (docs/PLAN.md F7): it is the
/// kernel's own estimate of what can be allocated without swapping, counting
/// the page cache it would reclaim. Nothing when the file has no MemAvailable
/// line -- kernels before 3.14 -- which is a different fact from a line that
/// is there and malformed, and is refused as one: this reader does not
/// estimate the figure the kernel did not give, because an estimate built
/// from MemFree is exactly the number F7 says not to use.
[[nodiscard]] core::Result<std::optional<std::uint64_t>, std::string> parse_mem_available(
    const std::vector<std::string>& lines);

/// One memory limit as the kernel prints it: v2's `memory.max`, where "max"
/// is no limit, or v1's byte count, where anything from kV1UnlimitedFloor up
/// is. Nothing for no limit.
[[nodiscard]] core::Result<std::optional<std::uint64_t>, std::string> parse_memory_limit(
    std::string_view text, CgroupVersion version);

/// The value of v1's `hierarchical_memory_limit` from the lines of a memory
/// cgroup's `memory.stat`, unparsed: the text after the key, for
/// parse_memory_limit. Refused when the line is missing or repeated -- every
/// v1 kernel since 2.6.29 writes it exactly once.
[[nodiscard]] core::Result<std::string, std::string_view> hierarchical_limit_text(
    const std::vector<std::string>& lines);

/// What this process may use: MemAvailable, and the memory limit that binds
/// its memory cgroup (docs/PLAN.md F7).
///
/// THE LIMIT THAT BINDS, NOT THE ONE ON THE LEAF
/// ---------------------------------------------
/// A limit binds every cgroup below it. A fresh v1 child of a limited cgroup
/// reads the unlimited sentinel in its own `memory.limit_in_bytes` while its
/// parent's 13 GiB still applies (verified on the machine this was written
/// on). So the leaf's own file is the wrong answer under both hierarchies.
///
/// v1 publishes the right one: `hierarchical_memory_limit` in the cgroup's
/// `memory.stat` is the kernel's own minimum over the cgroup and every
/// ancestor -- including ancestors above a container's mount root, which no
/// walk from inside the container can see. It is read and nothing is walked.
///
/// v2 publishes no effective figure, so every directory from the cgroup to
/// its mount is read and the lowest `memory.max` wins. What lies above the
/// mount's root is invisible from here; a container given its own subtree
/// as the mount sees the limit set on that subtree, which is the one its
/// runtime sets.
///
/// WHAT IS NOT READ, AND WHY
/// -------------------------
/// v2's `memory.high` is a throttle, not a wall: past it the kernel reclaims
/// and slows the cgroup, and nothing is killed. Usage is not subtracted: the
/// cgroup's usage counts page cache the kernel would reclaim before killing
/// anything, so limit minus usage understates what is available, and the
/// kernel publishes no per-cgroup MemAvailable to do better with. The
/// headroom F7 asks for is a configured number, not a guess made here.
///
/// A failure, with the source and the reason, whenever the budget cannot be
/// established: MemAvailable absent or unreadable, the cgroup not where its
/// own files say, a limit denied or malformed. Absent is a reading in exactly
/// two places -- no memory cgroup at all, and a v2 cgroup with no
/// `memory.max` (the hierarchy's root, or a cgroup whose parent does not
/// enable the memory controller for it) -- and both mean no limit is set
/// there. Everywhere else, "could not read" never becomes "no limit": that
/// would size a run against the host's memory inside a container with a
/// nearer wall.
[[nodiscard]] core::Result<config::MemoryBudget, topology::DiscoveryFailure> read_memory_budget(
    platform::FileSystem& filesystem, std::string_view root);

}  // namespace loadforge::resources

#endif
