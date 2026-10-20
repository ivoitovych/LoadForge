// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_RESOURCES_OOM_KILL_RECORD_HPP
#define LOADFORGE_RESOURCES_OOM_KILL_RECORD_HPP

#include <cstdint>
#include <string>

#include "core/result.hpp"

namespace loadforge::resources {

/// THE CONTRACT between whatever reads the kernel's record of OOM kills and
/// the controller that consults it (docs/adr/0003). The controller knows this
/// header and nothing behind it; it tests its side against a fake of its own,
/// and the reader tests this side against fixture trees and the real kernel.
///
/// WHAT THE COUNT IS
/// -----------------
/// The number of processes the kernel's OOM killer has killed within a scope
/// -- for the cgroup reader, the memory cgroup this process is in -- counted
/// since the scope came to exist. A consumer may rely on exactly this:
///
///   * It counts kills, not pids. Nothing in it says WHICH process died; two
///     deaths and one count leave the attribution open.
///   * It never decreases while the scope exists. A later reading below an
///     earlier one means the two were not read from the same scope.
///   * Every process the caller forks is inside the scope, unless something
///     moves it out: a child is born into its parent's cgroup.
///   * It may count processes the caller did not start: anything else that
///     shares the cgroup.
///   * A kill is counted before its victim can be reaped. The kernel counts
///     when it chooses the victim, before it sends the SIGKILL, so a reading
///     taken after a reap includes that reap's kill if there was one.
///
/// Reading it is not free and can fail; a failure is a reason in words a
/// report can carry, and is never zero.
class OomKillRecord {
 public:
  OomKillRecord() = default;
  OomKillRecord(const OomKillRecord&) = delete;
  OomKillRecord(OomKillRecord&&) = delete;
  OomKillRecord& operator=(const OomKillRecord&) = delete;
  OomKillRecord& operator=(OomKillRecord&&) = delete;
  virtual ~OomKillRecord() = default;

  /// What the count covers, in words a report can carry -- the file it is
  /// read from, or what was looked for when there is none.
  [[nodiscard]] virtual std::string scope() const = 0;

  /// The count now, or why it cannot be read now.
  [[nodiscard]] virtual core::Result<std::uint64_t, std::string> kills() = 0;
};

}  // namespace loadforge::resources

#endif
