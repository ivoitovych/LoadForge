// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_COUNTERS_COUNTERS_HPP
#define LOADFORGE_TELEMETRY_SOURCES_COUNTERS_COUNTERS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// What a failed perf_event_open(2) means for the counter it was asked for.
///
/// The man page documents sixteen errnos; they fall into three kinds of fact.
/// A refusal of permission (EACCES, EPERM) is the one the user can act on, and
/// the remedy is perf's own (below). A counter this machine cannot offer --
/// no PMU in a virtual machine (ENOENT), an event the CPU lacks (ENODEV,
/// EOPNOTSUPP), a kernel built without perf events (ENOSYS), an attribute it
/// rejects (EINVAL) -- is unsupported, and nothing the user does changes it.
/// Everything else (a full descriptor table, no free counter, a bad argument of
/// ours) is a failure of this attempt, not a fact about the machine, and is
/// kUnreadable rather than being passed off as absence.
[[nodiscard]] topology::Availability classify_perf_open(int error) noexcept;

/// What the user can do about a counter in the state it is in, for the
/// provider that failed: perf_event_open, whose gate is CAP_PERFMON and
/// kernel.perf_event_paranoid -- not file permissions, so never "run as root
/// to read a file" (PLAN F3).
///
/// `paranoid` is the level the kernel reports, when it could be read. Level 3
/// and above (a Debian and Ubuntu addition) refuses every unprivileged use,
/// so the advice is to lower it; at 2 or below an unprivileged process may
/// count itself in user space, so a refusal there comes from elsewhere -- a
/// container's seccomp profile or a security module -- and lowering the
/// sysctl would not help, which is why the advice differs.
[[nodiscard]] std::string perf_remediation(topology::Availability kind,
                                           std::optional<std::int64_t> paranoid,
                                           std::string_view subject);

/// Hardware and software performance counters (PLAN F3, F17), through
/// perf_event_open(2): `counters.cycles`, `counters.instructions`,
/// `counters.cache_misses`, `counters.branch_misses` (hardware) and
/// `counters.context_switches` (software), each a counter of events.
///
/// WHAT IT COUNTS. The calling process's main thread and every process it
/// forks after probe() -- the workers -- in user space only. User space because
/// that is what an unprivileged process may count at perf_event_paranoid 2, and
/// because a worker's work is its own instructions. Inheritance is the
/// kernel's: a counter opened with `inherit` follows fork(2).
///
/// WHEN THE PMU IS SHARED. More events than the PMU has counters are
/// time-shared, and the kernel reports how long each was enabled and how long
/// it ran. A reading is scaled by that ratio (an estimate, which is what perf
/// itself prints); one that never ran is reported unreadable rather than as
/// zero.
///
/// The descriptors are closed with the source. Thread-safety: none.
class CountersSource final : public Source {
 public:
  static constexpr std::string_view kDefaultProcRoot = "/proc";

  CountersSource(platform::Syscalls& syscalls, platform::FileSystem& filesystem,
                 std::string_view proc_root = kDefaultProcRoot);
  ~CountersSource() override;
  CountersSource(const CountersSource&) = delete;
  CountersSource(CountersSource&&) = delete;
  CountersSource& operator=(const CountersSource&) = delete;
  CountersSource& operator=(CountersSource&&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "counters"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  struct Channel {
    int fd = -1;
    std::string subject;  ///< "perf event type 0 config 0": for a message.
  };

  platform::Syscalls* syscalls_;
  platform::FileSystem* filesystem_;
  std::string paranoid_path_;
  std::optional<std::int64_t> paranoid_;
  std::vector<Channel> channels_;
};

}  // namespace loadforge::telemetry

#endif
