// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_PARSE_ERROR_HPP
#define LOADFORGE_CORE_PARSE_ERROR_HPP

#include <string_view>

namespace loadforge::core {

/// Why a textual configuration value could not be interpreted.
///
/// Kept deliberately specific: a configuration error is reported to a user who
/// must fix a file, so "invalid value" is not good enough.
enum class ParseError {
  Empty,          ///< The value was empty or entirely whitespace.
  InvalidNumber,  ///< The numeric part was missing or malformed.
  MissingUnit,    ///< A bare number was given where a unit is required.
  UnknownUnit,    ///< The unit suffix is not one this project accepts.
  NegativeValue,  ///< A negative value was given where only non-negative is meaningful.
  OutOfRange,     ///< The value does not fit, or falls outside the accepted range.
};

/// Human-readable explanation, suitable for putting in front of a user.
[[nodiscard]] std::string_view describe(ParseError error) noexcept;

}  // namespace loadforge::core

#endif
