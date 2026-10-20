// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/residual/residual.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include "core/result.hpp"

namespace loadforge::verification::residual {
namespace {

/// The smallest positive subnormal, 2^-1074: the unit U and V are counted in.
constexpr double kSubnormalUnit = 0x1p-1074;
/// up8 of the derivation: how many representable steps K is raised by.
constexpr int kRoundUpSteps = 8;

}  // namespace

Accumulation dot(std::span<const double> x, std::span<const double> y) noexcept {
  const std::size_t n = std::min(x.size(), y.size());
  Accumulation result;
  for (std::size_t i = 0; i < n; ++i) {
    const double product = x[i] * y[i];
    result.value += product;
    result.magnitude += std::abs(product);
  }
  return result;
}

Accumulation sum(std::span<const double> x) noexcept {
  Accumulation result;
  for (const double value : x) {
    result.value += value;
    result.magnitude += std::abs(value);
  }
  return result;
}

double gamma(std::size_t depth) noexcept {
  const double nu = static_cast<double>(depth) * kUnitRoundoff;
  return nu / (1.0 - nu);
}

std::string_view describe(Underivable why) noexcept {
  switch (why) {
    case Underivable::kNonFiniteMagnitude:
      return "the magnitude sum is not finite";
    case Underivable::kNegativeMagnitude:
      return "the magnitude sum is negative";
    case Underivable::kMagnitudeTooLarge:
      return "the magnitude sum exceeds 2^1000";
    case Underivable::kDepthTooLarge:
      return "the operation count exceeds 2^26";
  }
  return "unrecognised reason";
}

core::Result<double, Underivable> tolerance(ErrorModel model, double magnitude) noexcept {
  if (model.depth > kMaxDepth || model.products > kMaxDepth) {
    return Underivable::kDepthTooLarge;
  }
  if (!std::isfinite(magnitude)) {
    return Underivable::kNonFiniteMagnitude;
  }
  if (magnitude < 0.0) {
    return Underivable::kNegativeMagnitude;
  }
  if (magnitude > kMaxMagnitude) {
    return Underivable::kMagnitudeTooLarge;
  }
  const double g = gamma(model.depth);
  double k = 2.0 * g / (1.0 - g);
  for (int step = 0; step < kRoundUpSteps; ++step) {
    k = std::nextafter(k, std::numeric_limits<double>::infinity());
  }
  const double v = static_cast<double>(4 * model.products + 1) * kSubnormalUnit;
  return k * magnitude + v;
}

Comparison compare(double computed, double reference, double tolerance) noexcept {
  return Comparison{std::abs(computed - reference), tolerance};
}

bool within(const Comparison& comparison) noexcept {
  return comparison.residual <= comparison.tolerance;
}

}  // namespace loadforge::verification::residual
