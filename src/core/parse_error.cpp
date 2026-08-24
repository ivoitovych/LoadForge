// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/parse_error.hpp"

namespace loadforge::core {

std::string_view describe(ParseError error) noexcept {
  switch (error) {
    case ParseError::Empty:
      return "value is empty";
    case ParseError::InvalidNumber:
      return "value does not begin with a valid number";
    case ParseError::MissingUnit:
      return "value needs a unit suffix";
    case ParseError::UnknownUnit:
      return "unit suffix is not recognised";
    case ParseError::NegativeValue:
      return "value must not be negative";
    case ParseError::OutOfRange:
      return "value is out of the accepted range";
  }
  // Reached only if a value outside the enumerators is passed in. Kept total
  // rather than UB, and covered by a test, so adding an enumerator without
  // updating this function degrades gracefully instead of falling off the end.
  return "unrecognised parse error";
}

}  // namespace loadforge::core
