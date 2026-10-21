// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <string>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "workloads/mixed/mixed.hpp"
#include "workloads/workload.hpp"

namespace loadforge::mixed {

bool operator==(const RosterEntry& lhs, const RosterEntry& rhs) {
  return lhs.name == rhs.name && lhs.min_footprint_bytes == rhs.min_footprint_bytes;
}

std::vector<RosterEntry> roster_of(const workloads::Registry& registry,
                                   std::uint64_t ceiling_bytes) {
  constexpr std::uint64_t kLeast = std::uint64_t{64} << 10U;
  std::vector<RosterEntry> roster;
  for (const std::string& name : registry.names()) {
    for (std::uint64_t footprint = kLeast; footprint <= ceiling_bytes; footprint *= 2) {
      const workloads::WorkloadConfig config{1, footprint, 0, simd::IsaPath::kScalar};
      if (registry.create(name, config).has_value()) {
        roster.push_back(RosterEntry{name, footprint});
        break;
      }
    }
  }
  return roster;
}

}  // namespace loadforge::mixed
