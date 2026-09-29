// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/affinity.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

std::vector<std::uint32_t> ids_of(const CpuMask& mask) {
  std::vector<std::uint32_t> ids;
  for (std::size_t cpu = 0; cpu < kAffinityMaskBits; ++cpu) {
    if (mask.cpus.test(cpu)) {
      ids.push_back(static_cast<std::uint32_t>(cpu));
    }
  }
  return ids;
}

core::Result<CpuMask, std::string_view> mask_of(const std::vector<std::uint32_t>& ids) {
  CpuMask mask;
  for (const std::uint32_t id : ids) {
    if (id >= kAffinityMaskBits) {
      return std::string_view{"a CPU id past what an affinity mask can hold"};
    }
    mask.cpus.set(id);
  }
  return mask;
}

std::string describe(const CpuMask& mask) {
  std::string text;
  for (const std::uint32_t id : ids_of(mask)) {
    if (!text.empty()) {
      text += ',';
    }
    text += std::to_string(id);
  }
  // An if, not a ternary: a ternary with an allocating arm builds it inside one
  // expression, and the cleanup path that comes with that carries a branch no
  // test can take (journal §1.13).
  if (text.empty()) {
    return "(no CPUs)";
  }
  return text;
}

CpuMask missing_from(const CpuMask& wanted, const CpuMask& permitted) {
  CpuMask missing;
  missing.cpus = wanted.cpus & ~permitted.cpus;
  return missing;
}

std::string_view describe(Placement placement) noexcept {
  switch (placement) {
    case Placement::kNoUsableCpu:
      return "no CPU in the mask is one this thread may run on";
    case Placement::kThreadGone:
      return "the thread no longer exists";
    case Placement::kNotPermitted:
      return "not permitted to move this thread";
    case Placement::kUnexpected:
      return "the kernel refused for a reason this code does not expect";
  }
  // Reached only for a value outside the enumeration, which an enum's value
  // range permits. Saying so beats a blank a report would render as nothing.
  return "unrecognised placement failure";
}

Placement classify_placement(const SyscallError& error) noexcept {
  switch (error.number) {
    case EINVAL:
      // The kernel's one word for three facts: the mask is empty, names only
      // CPUs the machine lacks, or names only CPUs the cpuset withholds.
      // Verified on this kernel for all three. The mask in the message is what
      // lets a reader tell which.
      return Placement::kNoUsableCpu;
    case ESRCH:
      return Placement::kThreadGone;
    case EPERM:
      return Placement::kNotPermitted;
    default:
      return Placement::kUnexpected;
  }
}

std::string describe(const PlacementRefused& refused) {
  return "cannot place the thread on CPU(s) " + describe(refused.requested) + ": " +
         std::string{describe(refused.kind)} + " (" + describe(refused.cause) + ")";
}

core::Result<CpuMask, SyscallError> Affinity::permitted(pid_t pid) {
  return syscalls_->get_affinity(pid);
}

core::Result<core::Ok, PlacementRefused> Affinity::place(const CpuMask& mask, pid_t pid) {
  auto placed = syscalls_->set_affinity(pid, mask);
  if (placed.has_value()) {
    return core::Ok{};
  }
  // Built from named locals (journal §1.13): the cause is copied out first so
  // the aggregate constructs from a move and a trivially-copyable mask.
  SyscallError cause = placed.error();
  const Placement kind = classify_placement(cause);
  return PlacementRefused{kind, mask, std::move(cause)};
}

}  // namespace loadforge::platform
