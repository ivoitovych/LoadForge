// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/duration.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>

namespace loadforge::core {
namespace {

struct Unit {
  std::string_view suffix;
  std::uint64_t millis;
};

// Ordered longest-suffix-first so that "ms" is matched before "m".
constexpr std::array<Unit, 4> kUnits{{
    {"ms", 1},
    {"s", 1000},
    {"m", 60 * 1000},
    {"h", 60 * 60 * 1000},
}};

constexpr std::string_view kSpace = " \t";

std::string_view trim(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kSpace);
  return text.substr(first, last - first + 1);
}

}  // namespace

Result<Duration, ParseError> parse_duration(std::string_view text) {
  const std::string_view trimmed = trim(text);
  if (trimmed.empty()) {
    return ParseError::Empty;
  }
  if (trimmed.front() == '-') {
    return ParseError::NegativeValue;
  }

  std::uint64_t count = 0;
  const char* const begin = trimmed.data();
  const char* const end = begin + trimmed.size();
  const auto [stop, ec] = std::from_chars(begin, end, count);

  if (ec == std::errc::result_out_of_range) {
    return ParseError::OutOfRange;
  }
  if (ec != std::errc{}) {
    return ParseError::InvalidNumber;
  }

  const std::string_view suffix = trimmed.substr(static_cast<std::size_t>(stop - begin));
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
  auto millis = static_cast<std::uint64_t>(duration.count());
  // Largest exact unit first, so the output round-trips through parse_duration.
  for (std::size_t i = kUnits.size(); i-- > 0;) {
    const Unit& unit = kUnits[i];
    if (millis != 0 && millis % unit.millis == 0) {
      return std::to_string(millis / unit.millis) + std::string(unit.suffix);
    }
  }
  return std::to_string(millis) + "ms";
}

}  // namespace loadforge::core
