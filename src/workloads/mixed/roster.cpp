// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "workloads/mixed/mixed.hpp"
#include "workloads/workload.hpp"

namespace loadforge::mixed {

namespace {

/// Whether the registry makes `name` at `footprint` bytes.
bool creatable(const workloads::Registry& registry, const std::string& name,
               std::uint64_t footprint) {
  const workloads::WorkloadConfig config{1, footprint, 0, simd::IsaPath::kScalar};
  const auto made = registry.create(name, config);
  return made.has_value();
}

}  // namespace

bool operator==(const RosterEntry& lhs, const RosterEntry& rhs) {
  return lhs.name == rhs.name && lhs.min_footprint_bytes == rhs.min_footprint_bytes;
}

std::vector<RosterEntry> roster_of(const workloads::Registry& registry,
                                   std::uint64_t ceiling_bytes) {
  constexpr std::uint64_t kLeast = std::uint64_t{64} << 10U;
  std::vector<RosterEntry> roster;
  for (const std::string& name : registry.names()) {
    for (std::uint64_t footprint = kLeast; footprint <= ceiling_bytes; footprint *= 2) {
      if (creatable(registry, name, footprint)) {
        RosterEntry entry;
        entry.name = name;
        entry.min_footprint_bytes = footprint;
        roster.push_back(std::move(entry));
        break;
      }
      if (footprint > ceiling_bytes / 2) {
        break;  // The next doubling is past the ceiling, and past 64 bits for a huge one.
      }
    }
  }
  return roster;
}

}  // namespace loadforge::mixed
