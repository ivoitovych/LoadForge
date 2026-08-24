// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/byte_size.hpp"

#include <array>
#include <limits>
#include <ranges>

#include "core/parse_number.hpp"

namespace loadforge::core {
namespace {

struct Unit {
  std::string_view suffix;
  std::uint64_t scale;
};

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
constexpr std::uint64_t kGiB = 1024 * kMiB;
constexpr std::uint64_t kTiB = 1024 * kGiB;

constexpr std::array<Unit, 5> kUnits{{
    {"B", 1},
    {"KiB", kKiB},
    {"MiB", kMiB},
    {"GiB", kGiB},
    {"TiB", kTiB},
}};

constexpr std::uint64_t kMaxPercent = 100;

}  // namespace

Result<ByteSize, ParseError> parse_byte_size(std::string_view text) {
  const auto split = split_number_and_suffix(text);
  if (!split) {
    return split.error();
  }
  const std::uint64_t count = split.value().value;
  const std::string_view suffix = split.value().suffix;

  if (suffix.empty()) {
    return ByteSize::bytes(count);  // a bare number is a byte count
  }
  if (suffix == "%") {
    if (count == 0 || count > kMaxPercent) {
      return ParseError::OutOfRange;
    }
    return ByteSize::percent(count);
  }

  for (const Unit& unit : kUnits) {
    if (suffix != unit.suffix) {
      continue;
    }
    constexpr auto kCeiling = std::numeric_limits<std::uint64_t>::max();
    if (count > kCeiling / unit.scale) {
      return ParseError::OutOfRange;
    }
    return ByteSize::bytes(count * unit.scale);
  }
  return ParseError::UnknownUnit;
}

std::string to_string(ByteSize size) {
  if (!size.is_absolute()) {
    return std::to_string(size.percent_of_available()) + "%";
  }
  const std::uint64_t count = size.byte_count();
  for (const Unit& unit : std::views::reverse(kUnits)) {
    if (count != 0 && count % unit.scale == 0) {
      return std::to_string(count / unit.scale) + std::string(unit.suffix);
    }
  }
  return std::to_string(count) + "B";
}

}  // namespace loadforge::core
