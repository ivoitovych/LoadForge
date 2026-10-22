// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_INTEGRITY_LOCKING_HPP
#define LOADFORGE_VERIFICATION_INTEGRITY_LOCKING_HPP

// One mapping, made under the setup's lock policy (runner.hpp, LockPolicy):
// the working set and each copy of the control state go through here, so the
// policy means the same for both.

#include <string>

#include "core/result.hpp"
#include "platform/memory.hpp"
#include "verification/runner.hpp"

namespace loadforge::verification::integrity {

/// A mapping, and what was done about locking it.
struct Placed {
  platform::Mapping mapping;
  LockOutcome outcome = LockOutcome::kLocked;
  std::string detail;  ///< The refusal in words when the outcome is kFellBack, else empty.
};

/// Maps `request` (its `lock` member is ignored: the policy decides it).
///
///   kRequire  locked, or the refusal as it came.
///   kNone     not locked; kNotLocked.
///   kTry      locked; when the kernel refuses at the lock step (and only
///             then), the same request without the lock: kFellBack, with the
///             first refusal's words as the detail. A refusal at any other
///             step, or of the second attempt, is returned as it came.
[[nodiscard]] core::Result<Placed, platform::MappingRefused> map_by_policy(
    platform::Memory& memory, platform::MappingRequest request, LockPolicy policy);

}  // namespace loadforge::verification::integrity

#endif
