// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_RESOURCES_CGROUP_OOM_RECORD_HPP
#define LOADFORGE_RESOURCES_CGROUP_OOM_RECORD_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "resources/cgroup.hpp"
#include "resources/oom_kill_record.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::resources {

/// The file a memory cgroup keeps its OOM-kill count in: v2's
/// `memory.events`, v1's `memory.oom_control`. Both carry it as a line
/// "oom_kill N", since Linux 4.13.
[[nodiscard]] std::string_view oom_count_file(CgroupVersion version) noexcept;

/// The `oom_kill` count from the lines of either file. Nothing when there is
/// no such line -- a kernel before 4.13 keeps the file without the count --
/// which is a different fact from a line that is there and malformed.
[[nodiscard]] core::Result<std::optional<std::uint64_t>, std::string> parse_oom_kill_count(
    const std::vector<std::string>& lines);

/// The OOM-kill count of this process's own memory cgroup: the record the
/// controller consults to tell an OOM kill from any other SIGKILL.
///
/// WHY THE CGROUP'S COUNT, AND NOT /proc/vmstat
/// --------------------------------------------
/// /proc/vmstat's `oom_kill` counts every OOM kill on the machine. A rise in
/// it while a worker died of SIGKILL says the kernel killed SOMETHING, which
/// on a shared machine is a coincidence waiting to be reported as a finding.
/// The cgroup's count covers only the processes in this process's cgroup --
/// the workers among them, since a child is born into its parent's cgroup --
/// and it counts a victim of the machine-wide OOM killer as well as one of the
/// cgroup's own limit (verified on v1: a memcg OOM kill raised the victim's
/// cgroup count and /proc/vmstat's together). It is the narrowest record the
/// kernel keeps, short of a cgroup per worker.
///
/// It is read from this process's cgroup itself, not an ancestor. On v1 the
/// count is the cgroup's own (verified: a kill in a child cgroup left the
/// parent's count at 0); on v2 `memory.events` also counts the cgroup's
/// descendants. Either way the workers' kills are in it.
///
/// Located once, at construction; read on every call, through one Source, so
/// that a count that was read and is gone now is reported as vanished rather
/// than as a kernel that never kept one.
class CgroupOomKillRecord final : public OomKillRecord {
 public:
  CgroupOomKillRecord(platform::FileSystem& filesystem, std::string_view root);

  [[nodiscard]] std::string scope() const override;
  [[nodiscard]] core::Result<std::uint64_t, std::string> kills() override;

  /// The same reading, with the failure as the fact it is rather than as
  /// words: which source, and which of the capability states.
  [[nodiscard]] core::Result<std::uint64_t, topology::DiscoveryFailure> read();

 private:
  std::optional<topology::Source> count_;  ///< The count's file, once located.
  topology::DiscoveryFailure unlocated_;   ///< Why there is none, when there is none.
};

}  // namespace loadforge::resources

#endif
