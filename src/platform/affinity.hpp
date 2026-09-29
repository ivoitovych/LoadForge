// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_AFFINITY_HPP
#define LOADFORGE_PLATFORM_AFFINITY_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// The CPU ids a mask names, ascending.
[[nodiscard]] std::vector<std::uint32_t> ids_of(const CpuMask& mask);

/// A mask naming exactly these CPUs. Ids at or past kAffinityMaskBits are
/// refused rather than silently dropped: a mask missing a CPU the caller named
/// is a plausible wrong answer, and a thread pinned to "CPU 5000" that quietly
/// became "no CPU" would be the kind of bug that surfaces as a hang.
[[nodiscard]] core::Result<CpuMask, std::string_view> mask_of(
    const std::vector<std::uint32_t>& ids);

/// Renders a mask as its CPU ids, comma-separated, for messages.
[[nodiscard]] std::string describe(const CpuMask& mask);

/// The CPUs in `wanted` that `permitted` does not include.
///
/// This is the capability question affinity exists to answer. The topology
/// says which CPUs the machine HAS; the cpuset says which this process may
/// USE; and on a container, a systemd slice or a partitioned server the second
/// is narrower than the first. A workload that placed a worker on a CPU the
/// topology found and the cpuset withheld would be refused by the kernel -- or
/// worse, silently confined to whatever intersection remained. Asking first
/// turns that into a reported fact.
[[nodiscard]] CpuMask missing_from(const CpuMask& wanted, const CpuMask& permitted);

/// Why the kernel would not place a thread where it was asked.
///
/// Each errno sched_setaffinity(2) documents means something different to the
/// caller, and collapsing them would put the wrong advice in front of the
/// wrong reader:
///
///   kNoUsableCpu    EINVAL: nothing in the mask is a CPU this thread may run
///                   on -- absent from the machine, offline, or withheld by
///                   the cpuset. The remedy is a different mask, or a wider
///                   cpuset, and the message names the CPUs that were asked.
///   kThreadGone     ESRCH: there is no such thread. For a worker that means
///                   it died between being spawned and being placed, which is
///                   a supervision event, not a placement one.
///   kNotPermitted   EPERM: another user's thread, or a capability we lack.
///   kUnexpected     anything else. EFAULT would mean a bug in this code.
enum class Placement : std::uint8_t { kNoUsableCpu, kThreadGone, kNotPermitted, kUnexpected };

/// The word for a Placement, for messages.
[[nodiscard]] std::string_view describe(Placement placement) noexcept;

/// Classifies a failed sched_setaffinity(2). Pure and exposed, so the table
/// of errno-to-meaning is tested directly rather than through a fake.
[[nodiscard]] Placement classify_placement(const SyscallError& error) noexcept;

/// A refused placement: what kind, what was asked, and the kernel's own words.
struct PlacementRefused {
  Placement kind = Placement::kUnexpected;
  CpuMask requested;
  SyscallError cause;

  [[nodiscard]] friend bool operator==(const PlacementRefused&, const PlacementRefused&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const PlacementRefused& refused);

/// Reads and sets where a thread may run.
///
/// WHAT IS NOT CHECKED HERE, DELIBERATELY
/// --------------------------------------
/// `place` does not refuse an empty mask, or a mask naming a CPU past what the
/// machine has, before asking the kernel. It could; it would be cheap; and it
/// would mean this code deciding what the kernel will accept instead of
/// asking. The kernel is the authority on the cpuset, on which CPUs are
/// online this instant, and on whether the thread still exists -- and every
/// one of those can change between a check here and the call. So the call is
/// always made, and the kernel's answer is classified, never pre-empted. The
/// one thing that IS decided above the seam is the meaning of the answer.
///
/// Thread-safety: holds no state beyond the Syscalls pointer; as shareable as
/// that is. RealSyscalls is stateless.
class Affinity {
 public:
  explicit Affinity(Syscalls& syscalls) : syscalls_(&syscalls) {}

  /// The CPUs the thread may currently run on -- the cpuset's answer, not the
  /// topology's. pid 0 is the calling thread.
  [[nodiscard]] core::Result<CpuMask, SyscallError> permitted(pid_t pid = 0);

  /// Confines the thread to `mask`, or says why the kernel would not.
  ///
  /// On success the thread runs on the INTERSECTION of `mask` and the cpuset,
  /// which the kernel computes and which may be narrower than `mask`. A caller
  /// that needs to know what it actually got reads `permitted` afterwards; a
  /// caller that needs to know beforehand asks `missing_from` first.
  [[nodiscard]] core::Result<core::Ok, PlacementRefused> place(const CpuMask& mask, pid_t pid = 0);

 private:
  Syscalls* syscalls_;
};

}  // namespace loadforge::platform

#endif
