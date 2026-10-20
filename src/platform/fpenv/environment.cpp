// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/fpenv/environment.hpp"

#include <string>

namespace loadforge::fpenv {

core::Result<core::Ok, FpError> assert_environment(const FpControl& control) {
  return check(control.read());
}

core::Result<core::Ok, FpError> establish_environment(FpControl& control) {
  const FpState found = control.read();
  control.write(
      required_state(found.denormals_are_zero.has_value(), found.default_nan.has_value()));
  return assert_environment(control);
}

std::string describe(const FpError& error) {
  std::string message = "the floating-point environment is not the required one (";
  bool first = true;
  for (const FpViolation& violation : error.violations) {
    if (!first) {
      message += ", ";
    }
    first = false;
    message += violation.property + " is " + violation.found;
  }
  message +=
      "); required: round to nearest with ties to even, and every flush-to-zero, "
      "denormals-are-zero "
      "and default-NaN control this architecture has, off";
  return message;
}

}  // namespace loadforge::fpenv
