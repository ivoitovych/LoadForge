// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/integrity/difference.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace loadforge::verification::integrity {

std::optional<Difference> compare_stored(std::span<const std::byte> expected,
                                         std::span<const std::byte> actual) {
  // The common case is equality, and memcmp is how a slice of large units
  // stays proportional to its work rather than to a byte loop's.
  if (std::memcmp(expected.data(), actual.data(), expected.size()) == 0) {
    return std::nullopt;
  }

  Difference difference;
  difference.domain = Domain::kStored;
  bool first = true;
  for (std::size_t at = 0; at < expected.size(); ++at) {
    const auto delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
    if (delta == 0) {
      continue;
    }
    if (first) {
      difference.first_offset = at;
      first = false;
    }
    difference.last_offset = at;
    difference.popcount += static_cast<std::uint64_t>(std::popcount(delta));
  }

  difference.window_offset = difference.first_offset / kWordBytes * kWordBytes;
  const std::size_t window_end =
      std::min<std::size_t>(expected.size(), difference.window_offset + kMaxWindowBytes);
  const auto from = static_cast<std::ptrdiff_t>(difference.window_offset);
  const auto to = static_cast<std::ptrdiff_t>(window_end);
  difference.expected.assign(expected.begin() + from, expected.begin() + to);
  difference.actual.assign(actual.begin() + from, actual.begin() + to);
  return difference;
}

}  // namespace loadforge::verification::integrity
