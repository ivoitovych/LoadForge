// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/byte_size.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <system_error>

namespace loadforge::core {
namespace {

struct Unit {
  std::string_view suffix;
  std::uint64_t scale;
};

constexpr std::uint64_t kKiB = 1024;
constexpr std::array<Unit, 5> kUnits{{
    {"B", 1},
    {"KiB", kKiB},
    {"MiB", kKiB* kKiB},
    {"GiB", kKiB* kKiB* kKiB},
    {"TiB", kKiB* kKiB* kKiB* kKiB},
}};

constexpr std::uint64_t kMaxPercent = 100;
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

Result<ByteSize, ParseError> parse_byte_size(std::string_view text) {
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
  for (std::size_t i = kUnits.size(); i-- > 0;) {
    const Unit& unit = kUnits[i];
    if (count != 0 && count % unit.scale == 0) {
      return std::to_string(count / unit.scale) + std::string(unit.suffix);
    }
  }
  return std::to_string(count) + "B";
}

}  // namespace loadforge::core
