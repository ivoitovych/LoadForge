// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "platform/fpenv/fpenv.hpp"

namespace loadforge::fpenv {

core::Result<core::Ok, FpError> check(const FpState& state) {
  std::vector<FpViolation> violations;
  if (state.rounding != Rounding::kToNearestEven) {
    violations.push_back({"rounding", std::string(describe(state.rounding))});
  }
  if (state.flush_to_zero) {
    violations.push_back({"flush_to_zero", "on"});
  }
  if (state.denormals_are_zero.value_or(false)) {
    violations.push_back({"denormals_are_zero", "on"});
  }
  if (state.default_nan.value_or(false)) {
    violations.push_back({"default_nan", "on"});
  }
  if (violations.empty()) {
    return core::Ok{};
  }
  return FpError{std::move(violations)};
}

}  // namespace loadforge::fpenv
