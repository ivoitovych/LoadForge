// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_PARSE_NUMBER_HPP
#define LOADFORGE_CORE_PARSE_NUMBER_HPP

#include <cstdint>
#include <string_view>

#include "core/parse_error.hpp"
#include "core/result.hpp"

namespace loadforge::core {

/// A textual value split into its leading number and its trailing unit.
struct NumberAndSuffix {
  std::uint64_t value;
  std::string_view suffix;

  friend constexpr bool operator==(const NumberAndSuffix&,
                                   const NumberAndSuffix&) noexcept = default;
};

/// Strip leading and trailing spaces and tabs. TOML values are frequently
/// padded, and rejecting "  20m" for whitespace would be unhelpful.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

/// Split a trimmed value into a non-negative integer and whatever follows it.
///
/// Shared by every unit-suffixed configuration type, so the rules about empty
/// input, sign, malformed digits and uint64 overflow are decided in exactly one
/// place rather than drifting between them.
[[nodiscard]] Result<NumberAndSuffix, ParseError> split_number_and_suffix(std::string_view text);

}  // namespace loadforge::core

#endif
