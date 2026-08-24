// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/parse_number.hpp"

#include <charconv>
#include <system_error>

namespace loadforge::core {
namespace {
constexpr std::string_view kSpace = " \t";
}  // namespace

std::string_view trim(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kSpace);
  return text.substr(first, last - first + 1);
}

Result<NumberAndSuffix, ParseError> split_number_and_suffix(std::string_view text) {
  const std::string_view trimmed = trim(text);
  if (trimmed.empty()) {
    return ParseError::Empty;
  }
  if (trimmed.front() == '-') {
    return ParseError::NegativeValue;
  }

  std::uint64_t value = 0;
  const char* const begin = trimmed.data();
  // std::from_chars takes a pointer pair; a string_view cannot express that
  // without arithmetic on its data pointer. Bounded by size(), so it stays
  // within the view.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char* const end = begin + trimmed.size();
  const auto [stop, ec] = std::from_chars(begin, end, value);

  if (ec == std::errc::result_out_of_range) {
    return ParseError::OutOfRange;
  }
  if (ec != std::errc{}) {
    return ParseError::InvalidNumber;
  }
  return NumberAndSuffix{value, trimmed.substr(static_cast<std::size_t>(stop - begin))};
}

}  // namespace loadforge::core
