// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/duration.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <ranges>

#include "core/parse_number.hpp"

namespace loadforge::core {
namespace {

struct Unit {
  std::string_view suffix;
  std::uint64_t millis;
};

constexpr std::uint64_t kSecond = 1000;
constexpr std::uint64_t kMinute = 60 * kSecond;
constexpr std::uint64_t kHour = 60 * kMinute;

// Ordered longest-suffix-first so that "ms" is matched before "m".
constexpr std::array<Unit, 4> kUnits{{
    {"ms", 1},
    {"s", kSecond},
    {"m", kMinute},
    {"h", kHour},
}};

}  // namespace

Result<Duration, ParseError> parse_duration(std::string_view text) {
  const auto split = split_number_and_suffix(text);
  if (!split) {
    return split.error();
  }
  const std::uint64_t count = split.value().value;
  const std::string_view suffix = split.value().suffix;

  if (suffix.empty()) {
    return ParseError::MissingUnit;
  }

  for (const Unit& unit : kUnits) {
    if (suffix != unit.suffix) {
      continue;
    }
    constexpr auto kCeiling = static_cast<std::uint64_t>(std::numeric_limits<Duration::rep>::max());
    if (count > kCeiling / unit.millis) {
      return ParseError::OutOfRange;
    }
    return Duration{static_cast<Duration::rep>(count * unit.millis)};
  }
  return ParseError::UnknownUnit;
}

std::string to_string(Duration duration) {
  const auto millis = static_cast<std::uint64_t>(duration.count());
  // Largest exact unit first, so the output round-trips through parse_duration.
  for (const Unit& unit : std::views::reverse(kUnits)) {
    if (millis != 0 && millis % unit.millis == 0) {
      return std::to_string(millis / unit.millis) + std::string(unit.suffix);
    }
  }
  return std::to_string(millis) + "ms";
}

}  // namespace loadforge::core
