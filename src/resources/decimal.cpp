// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources/decimal.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace loadforge::resources {

std::optional<std::uint64_t> parse_decimal(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    // value * 10 + digit <= kMax exactly when value <= (kMax - digit) / 10.
    if (value > (kMax - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value;
}

}  // namespace loadforge::resources
