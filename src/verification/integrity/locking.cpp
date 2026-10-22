// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/integrity/locking.hpp"

#include <utility>

namespace loadforge::verification::integrity {

core::Result<Placed, platform::MappingRefused> map_by_policy(platform::Memory& memory,
                                                             platform::MappingRequest request,
                                                             LockPolicy policy) {
  request.lock = policy != LockPolicy::kNone;
  auto mapped = memory.map(request);
  if (mapped.has_value()) {
    Placed placed{std::move(mapped).value(), LockOutcome::kLocked, {}};
    if (policy == LockPolicy::kNone) {
      placed.outcome = LockOutcome::kNotLocked;
    }
    return placed;
  }
  if (policy != LockPolicy::kTry || mapped.error().stage != platform::Stage::kLock) {
    return mapped.error();
  }
  std::string refusal = platform::describe(mapped.error());
  request.lock = false;
  auto unlocked = memory.map(request);
  if (!unlocked.has_value()) {
    return unlocked.error();
  }
  return Placed{std::move(unlocked).value(), LockOutcome::kFellBack, std::move(refusal)};
}

}  // namespace loadforge::verification::integrity
